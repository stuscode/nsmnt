#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* nsmnt: run a program in a new namespace, mounting directories */
void usage()
{
   fprintf(stderr,"usage: nsmnt [-f file] [-m src=dest[=options]] [-t [+|-][yyyy[MM[dd[hh[mm[ss]]]]]]] [-h hostname] program [args]\n");
   fprintf(stderr,"-f: file of -m arguments, one per line\n");
   fprintf(stderr,"-m: mount src outside namespace to dest inside namespace\n");
   fprintf(stderr,"-t: set the time offset in the namespace\n");
   fprintf(stderr,"if + or - is specified, it is relative to current time\n");
   fprintf(stderr,"-h: set the hostname in the namespace\n");
   fprintf(stderr,"-d: print debug messages\n");
   exit(0);
}

/* function prototypes */
int pw_setup(int [2]);
int pw_wait(int [2]);
int pw_go(int [2]);
int pidone(void *);
void update_map(char *, pid_t, char *);
int parseandmount(char *);
unsigned long build_options(char *);
static int exectarget (void *);
char * find_equal_unquote(char *);
double time_offset (char *);
void setnstime(double , char *);

#define PIDONE_STACK_SIZE 1048576
#define TARGET_STACK_SIZE 1048576
static char pidone_stack[PIDONE_STACK_SIZE];
static char target_stack[TARGET_STACK_SIZE];

int pfd[2];
uid_t uid;  /* process uid.  Not doing setuid */
gid_t gid;  /* process gid.  Not doing setgid */

typedef struct 
{
   char *arg;
   unsigned long mask;
} mountopts;

/* mount options */
mountopts mount_options[] = 
{
	{"dirsync", MS_DIRSYNC},
	{"mandlock", MS_MANDLOCK},
	{"noatime", MS_NOATIME},
	{"nodev", MS_NODEV},
	{"nodiratime", MS_NODIRATIME},
	{"noexec", MS_NOEXEC},
	{"nosuid", MS_NOSUID},
	{"ro", MS_RDONLY},
	{"recursive", MS_REC},
	{"relatime", MS_RELATIME},
	{"silent", MS_SILENT},
	{"strictatime", MS_STRICTATIME},
	{"sync", MS_SYNCHRONOUS},
	{"remount", MS_REMOUNT},
	{"bind", MS_BIND},
	{"shared", MS_SHARED},
	{"private", MS_PRIVATE},
	{"slave", MS_SLAVE},
	{"unbindable", MS_UNBINDABLE},
	{"move", MS_MOVE},
        {NULL, 0},
};


int GLOBAL_DEBUG = 0;
void DBPRINT(char * format, ...)
{
    char buffer[4096];
    va_list args;
    if (GLOBAL_DEBUG)
    {
       va_start(args, format);
       vsprintf(buffer, format, args);
       printf("%s", buffer);
       va_end(args);
    }
}

 /* find a equal character, make sure it isn't in a quoted string */
char * find_equal_unquote(char *s)
{
   int inq = 0; /* if 0, not in quote */
   int pos = 0;

   while (s[pos] != '\0')
   {
      if (s[pos] == '\'' || s[pos] == '\"')
         inq ^= s[pos];
      if (s[pos] == '=')
         if (inq ==  0)
            return &(s[pos]);
      pos++;
   }
   return NULL; /* not found in string */
}

typedef struct
{
   char **maps; /*pointer to array of pointers to map strings */
   double toff;  /* use double to get known type */
   char *hostname;
   char **progargs; 
   char *program;
} arguments;

/* --------------------- PROCESS ARGUMENTS -------------------- */

  /* debug routine */
void dump_arguments(arguments *a)
{
   int mnum = 0;

   printf("time offset %.0f\n", a->toff);
   printf("hostname %s\n", a->hostname);
   while ((a->maps)[mnum] != NULL)
   {
      printf("map %s\n", (a->maps)[mnum]);
      mnum++;
   }
   printf("program %s\n", a->program);
   mnum = 0;
   while ((a->progargs)[mnum] != NULL)
   {
      printf("progarg %s\n", (a->progargs)[mnum]);
      mnum++;
   }
}

/* add map argument to list of maps */
void addmap(char ***a, char *mapstring)
{
   int mapnum = 0;
   char * curmap;
   char **alist;

   if (*a == NULL)
   {
      *a = malloc(sizeof(char *) * 2);
      alist = *a;
   }
   else
   {
      alist = *a;
      curmap = alist[mapnum];
      while (curmap != NULL)
      {
         mapnum++;
         curmap = alist[mapnum];
      }  /* count number in list */
         /* make new list 2 larger (new item plus ending NULL) */
      *a = realloc(*a, sizeof(char *) * (mapnum + 2));
      alist = *a;
   }
#ifdef OLD
   newstring = malloc(strlen(mapstring) + 1);
   strcpy(newstring, mapstring);
   alist[mapnum] = newstring;
#endif /*OLD*/
   alist[mapnum] = mapstring; /*save link to program arg */
   alist[mapnum + 1] = NULL;
}

/* TODO: expand relative path in file to full path from file location */
void addmapsfromfile(char ***a, char *file)
{
   char *line;
   size_t len;
   int l = 1;
   FILE *fs;

          /* open file */
   fs = fopen(file, "r");
   if (fs == NULL)
   {
      fprintf(stderr,"Couldn't open file %s as map file, errno=%d\n", 
                         file, errno);
      exit(1);
   }
   while (l > 0 )
   {
      len = 0;
      line = NULL;
          /* read lines from file, getline does malloc for us */
/*TODO: relative path is from where the config file is */
      l = getline(&line, &len, fs);
          /* add map for line */
      addmap(a, line); /*saves line */
   }
}

/* find options */
unsigned long findoption(char *opt)
{
   int i=0;

   while (mount_options[i].arg != NULL)
   {
      if (strcmp(mount_options[i].arg, opt) == 0)
         return mount_options[i].mask;
      i++;
   }
   return 0;
}

/* flags for mount() */
unsigned long build_options(char *opt)
{
/* from  /usr/include/x86_64-linux-gnu/sys/mount.h */
   char *pt;
   char *optcopy;
   unsigned long mask;
   unsigned long allmask = 0;
   
   optcopy = malloc(strlen(opt) + 1);
   strcpy(optcopy, opt);
   pt = strtok (optcopy,",");
   while (pt != NULL) 
   {
      mask = findoption(pt);
      allmask |= mask;
      if (mask == 0) /* not found */
      {
         fprintf(stderr,"warning: mount option %s not found.\n", pt);
      }
      pt = strtok (NULL, ",");
   }
   free(optcopy);
   return allmask;
}

/* return time offset that should be used for the new namespace */
/* it is in seconds.  The character string is in the following format: */
/*   [+|-][yyyy[MM[dd[hh[mm[ss]]]]]] */ 
/* return double because difftime is the only way to get a time that is a */
/* well defined size */
double time_offset(char *targ)
{
   time_t t1, now;
   int len;
   char tmp[8];
   struct tm tm;
   char *ret;

   len = strlen(targ);
   if (targ[0] == '-' || targ[0] == '+')
   {
      switch(len-1) 
      {
       case 14: /* YYYYMMddhhmmss */
         strncpy(tmp, &(targ[len-14]), 4);
         tmp[4] = '\0';
         t1 += (atoi(tmp) * 60 * 60 * 24 * 30 * 12);
       case 10: /* MMddhhmmss */
         strncpy(tmp, &(targ[len-10]), 2);
         tmp[2] = '\0';
         t1 += (atoi(tmp) * 60 * 60 * 24 * 30); /*generic 30 day month */
       case 8:  /* ddhhmmss */
         strncpy(tmp, &(targ[len-8]), 2);
         tmp[2] = '\0';
         t1 += (atoi(tmp) * 60 * 60 * 24);
       case 6:  /* hhmmss */
         strncpy(tmp, &(targ[len-6]), 2);
         tmp[2] = '\0';
         t1 += (atoi(tmp) * 60 * 60);
       case 4:  /* mmss */
         strncpy(tmp, &(targ[len-4]), 2);
         tmp[2] = '\0';
         t1 += (atoi(tmp) * 60);
       case 2:  /* ss */
         strncpy(tmp, &(targ[len-2]), 2);
         tmp[2] = '\0';
         t1 += atoi(tmp);
	 break;
       default:
	 fprintf(stderr, "%s is not a valid time specification\n", targ);
	 exit(1);
      }
      now = 0; /* t1 is already a difference, so we will diff it with 0 */
   }
   else /* find diff from now until actual date specified */
   {
      switch(len)
      {
       case 14: /* YYYYMMddhhmmss */
          ret = strptime(targ, "%Y%m%d%H%M%S", &tm);
	  break;
       case 10: /* MMddhhmmss */
          ret = strptime(targ, "%m%d%H%M%S", &tm);
	  break;
       case 8:  /* ddhhmmss */
          ret = strptime(targ, "%d%H%M%S", &tm);
	  break;
       case 6:  /* hhmmss */
          ret = strptime(targ, "%H%M%S", &tm);
	  break;
       case 4:  /* mmss */
          ret = strptime(targ, "%M%S", &tm);
	  break;
       case 2:  /* ss */
          ret = strptime(targ, "%S", &tm);
	  break;
       default:
	 fprintf(stderr, "%s is not a valid time specification\n", targ);
	 exit(1);
      }
      t1 = mktime(&tm);
      now = time(NULL);
   }
   return difftime(t1, now);
}

arguments *process_args(int argc, char *argv[])
{
   int argn = 1;
   int npargs;
   int index = 0;
   int nmap = 0; /* number of map arguments */
   arguments * args;

   args = malloc(sizeof(arguments));
   if (args == NULL)
   {
      fprintf(stderr,"can't malloc arguments\n");
      exit(1);
   }
   bzero(args, sizeof(arguments));
   args->toff = 0.0;
   args->maps = malloc(sizeof(void *)); /*always a list*/
   *(args->maps) = NULL;
#ifdef NOT
   args->progargs = malloc(sizeof(void *)); /*always a list*/
   *(args->progargs) = NULL;
#endif /*NOT*/
   while (argn <  argc)
   {
      if (argv[argn][0] == '-')
      {
         if (argv[argn][1] == 'f')
         {
            argn++;
            addmapsfromfile(&(args->maps), argv[argn]);
         }
         if (argv[argn][1] == 'm')
         {
            argn++;
            addmap(&(args->maps), argv[argn]);
         }
         else if (argv[argn][1] == 't')
         {
            argn++;
	    args->toff = time_offset(argv[argn]);
         }
         else if (argv[argn][1] == 'h')
         {
            argn++;
            args->hostname = malloc(strlen(argv[argn])+1);
            strcpy(args->hostname, argv[argn]);
         }
         else if (argv[argn][1] == 'd')
         {
            GLOBAL_DEBUG = 1;
         }
      }
      else /* process program and arguments */
      {
         args->program = argv[argn];
#ifdef NOT
         addmap(&(args->progargs), argv[argn]); /* program is first arg */
         argn++;

         while (argn < argc)
         {
            addmap(&(args->progargs), argv[argn]);
            argn++;
         }
         addmap(&(args->progargs), NULL);
         break; /* this else handled all the rest of the arguments */
#endif /*NOT*/
         args->progargs=&(argv[argn]);
         argn = argc;
      }
      argn++;
   }
   if (args->program == NULL)
   {
      usage();
   }
   return args;
}

/* -------------- END OF PROCESS ARGUMENTS -------------------- */

void setnstime(double toff, char *clock)
{
   int fd;
   char map[512];
   int maplen;

   fd = open("/proc/self/timens_offsets", O_RDWR);
   sprintf(map, "%s %.0f 0\n", clock, toff);
   maplen = strlen(map);
   if (write(fd, map, maplen) != maplen)
   {
      perror("set time");
      exit(1);
   }
   close(fd);
}

int main(int argc, char *argv[])
{
   pid_t child_pid;
   char map[512];
   int cloneflags;
   int unshareflags;
   int stat;
   arguments *args;

   pw_setup(pfd);
   uid = getuid();
   gid = getgid();
   args = process_args(argc, argv);
   if (GLOBAL_DEBUG)
      dump_arguments(args);
   cloneflags = CLONE_NEWNS | CLONE_NEWUSER | CLONE_NEWPID | SIGCHLD;
   if (args->hostname != NULL)
      cloneflags |=  CLONE_NEWUTS;
#ifdef CLONE_NEWTIME
   if (args->toff != 0.0)
   {
      unshareflags =  CLONE_NEWTIME;
      stat = unshare(unshareflags);
      if (stat != 0)
      {
         perror("unshare");
	 exit(1);
      }
          /* set time for children */
      /*TODO: different time specs for each clock */
      setnstime(args->toff, "monotonic");
      setnstime(args->toff, "boottime");

   }
#endif /*CLONE_NEWTIME*/
   child_pid = clone(pidone, pidone_stack + PIDONE_STACK_SIZE, cloneflags, args);
   if (child_pid < 0)
   {
      perror("clone");
      exit(1);
   }
   DBPRINT("child_pid: %ld\n", (long) child_pid);
   update_map("deny", child_pid, "setgroups");
   sprintf(map, "%ld %ld 1\n", (long) uid, (long) uid);
   update_map(map,    child_pid, "uid_map");
   sprintf(map, "%ld %ld 1\n", (long) gid, (long) gid);
   update_map(map,    child_pid, "gid_map");
   DBPRINT("go\n");
   pw_go(pfd);  /* release child in namespace */
   waitpid(child_pid, NULL, 0); /* wait for end */
   return 0;
}

void update_map(char *data, pid_t pid, char *map_file)
{
   int fd, j, maplen;
   char fname[512];

   sprintf(fname,"/proc/%ld/%s", (long)pid, map_file);
   maplen = strlen(data);
   fd = open(fname, O_RDWR);
   if (fd == -1)
   {
      fprintf(stderr,"open %s: %s\n", fname, strerror(errno));
      exit(EXIT_FAILURE);
   }
   if (write(fd, data, maplen) != maplen)
   {
      fprintf(stderr, "write %s: %s\n", fname, strerror(errno));
      exit(EXIT_FAILURE);
   }
   close(fd);
}


/* this is the first process created in the new namespaces */
/* it will set up the environment, then clone a new process */
/* which will then exec the target.  It will then wait and handle */
/* signals. */
void handle_sigchild(int sig)
{
    DBPRINT("Caught signal %d\n", sig);
}

void handle_sigterm(int sig)
{
   /* TODO End process */
    DBPRINT("Caught signal %d\n", sig);
/*    exit(0);*/
}


int pidone(void *a) 
{
   int mnum = 0;
   int res;
   arguments *args = (arguments *) a;
   struct sigaction sas;
   pid_t pid, primary_pid;

   DBPRINT("wait\n");
   pw_wait(pfd); /*wait for parent to set uid and gid maps */
   DBPRINT("done wait\n");
   res = mount("none", "/proc", "proc", 0, ""); /* mount /proc for new pid namespace*/
   if (res < 0)
   {
      perror("mount /proc");
      exit(1);  /* TODO: maybe this is optional */
   }
      /* set up namespace environment */
   if (args->hostname != NULL)
   {
      sethostname(args->hostname, strlen(args->hostname)); /*TODO: check return */
		     /* TODO: set domain name (optional) (with '.' in name) */
   }
   
   while ((args->maps)[mnum] != NULL)
   {
      parseandmount((args->maps)[mnum]);
      mnum++;
   }

   primary_pid = clone(exectarget, target_stack + TARGET_STACK_SIZE, SIGCHLD, (void *) args); 

   signal(SIGTERM, handle_sigterm); 
   signal(SIGCHLD, handle_sigchild); 

   while ((pid = wait(NULL)) > 0)
   {
      if (pid == primary_pid)
      {
         /* TODO: MAKE THIS EXIT THE PROCESS */
         /* start exit timer as immediate child exited */
         DBPRINT("exit of main proc %d\n", pid);
         return 0;
      }
      DBPRINT("exit %ld\n",(long)pid);
   }
   DBPRINT("final exit\n");
   return 0;
}

/* exec the target executable */
static int exectarget (void *args)
{
   arguments *a = (arguments *)args;

   DBPRINT("exectarget %s\n", a->program);
   execvp(a->program, a->progargs);
   fprintf(stderr, "Error execing primary executable\n");
   exit(1);
}

/* format: /source/path=/dest/path[:options]? */
/* options are comma separated */
int parseandmount(char *m)
{
   char *opts = NULL;
   char *destarg;
   char *arg, *source, *dest;
   unsigned long mntflags = 0;
   char * colpos;
   int stat; /*status from mount */

      /* copy option from command line arg to local storage, then 
       * section it into strings for the mount.  Then free it 
       */

      /*TODO error checking */
   arg = malloc(strlen(m + 1));
   if (arg == NULL)
   {
      perror("malloc arg");
      exit(1);
   }
   strcpy(arg, m);
   /*get source directory */
   source = arg;
   colpos = find_equal_unquote(arg);
   *colpos = '\0';  /* null terminate source */
   /*get destination directory */
   dest = colpos + 1;
   /*if there's another equal, then there are options */
   colpos = find_equal_unquote(dest);
   if (colpos != NULL) 
   {
      *colpos = '\0';  /* null terminate dest */
      opts = colpos+1; /* options should be already null terminated */
      mntflags = build_options(opts);
   }
   mntflags |= MS_BIND|MS_REC;
   stat  = mount(source, dest, NULL, mntflags, NULL);  
   if (stat != 0)
   {
      perror("mount");
      DBPRINT("source %s, dest %s, flags %ld\n",source, dest, mntflags);
      fprintf(stderr,"argument: %s\n", m);
      exit(1);
   }
   free(arg);
}


/* --------------------- SYNCRONIZATION ROUTINES -------------- */

int pw_wait(int pfd[2])
{
   char ch;
   close(pfd[1]);
   return read(pfd[0], &ch, 1); /* success returns 0 */
}

int pw_go(int pfd[2])
{
   return close(pfd[1]); /* success returns 0 */
}
int pw_setup(int pfd[2])
{
   return pipe(pfd); /* success returns 0 */
}
