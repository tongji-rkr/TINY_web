#include "csapp.h"
#include <sys/epoll.h>
#include <pthread.h>
#include <pthread.h>

#define THREAD_POOL_SIZE 4
#define MAX_TASKS 100

typedef struct {
    int fd;
    struct sockaddr_storage clientaddr;
    socklen_t clientlen;
} TaskData;

void *thread_worker(void *arg);
void enqueue_task(TaskData task);
TaskData dequeue_task();
void handle_request(TaskData task);
void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
char* get_client_address(struct sockaddr_storage clientaddr, socklen_t clientlen);

pthread_mutex_t task_mutex;
pthread_mutex_t thread_mutex;
pthread_cond_t task_cond;
pthread_t thread_pool[THREAD_POOL_SIZE];
TaskData task_queue[MAX_TASKS];
int task_count = 0;
int terminate_threads = 0;
int queue_front = 0;
int queue_rear = -1;
int isthreadpool = 1;

int main(int argc, char **argv)
{
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

    /* Check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    if(isthreadpool){
        // Initialize mutex and condition variable objects
        pthread_mutex_init(&task_mutex, NULL);
        pthread_mutex_init(&thread_mutex, NULL);
        pthread_cond_init(&task_cond, NULL);

        // Create thread pool
        for (int i = 0; i < THREAD_POOL_SIZE; i++){
            pthread_create(&thread_pool[i], NULL, thread_worker, NULL);
        }
    }

    listenfd = Open_listenfd(argv[1]);
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen); //line:netp:tiny:accept
            Getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE, 
                        port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);

        if(isthreadpool){
            // Enqueue the task
            enqueue_task((TaskData){connfd, clientaddr, clientlen});
            printf("task count : %d\n", task_count);
        } else {
            doit(connfd);                                             //line:netp:tiny:doit
            Close(connfd);                                            //line:netp:tiny:close
        }
    }

    if(isthreadpool) {
        // Wait for all threads to finish
        for (int i = 0; i < THREAD_POOL_SIZE; i++){
            pthread_join(thread_pool[i], NULL);
        }

        pthread_mutex_destroy(&task_mutex);
        pthread_mutex_destroy(&thread_mutex);
        pthread_cond_destroy(&task_cond);
    }
    return 0;
}

void *thread_worker(void *arg)
{
    printf("new pthread created\n");
    while (1)
    {
        TaskData task;

        pthread_mutex_lock(&task_mutex);

        // Wait for a task to be available
        while (task_count == 0 && !terminate_threads)
        {
            pthread_t tid = pthread_self();
            printf("Thread ID: %lu\n", (unsigned long)tid);
            pthread_cond_wait(&task_cond, &task_mutex);
        }

        // Check if threads should terminate
        if (terminate_threads)
        {
            pthread_mutex_unlock(&task_mutex);
            pthread_exit(NULL);
        }

        // Dequeue a task
        task = dequeue_task();
        printf("task : %d\n", task.fd);

        pthread_mutex_unlock(&task_mutex);

        // show tid
        pthread_t tid = pthread_self();
        printf("Thread ID: %lu\n", (unsigned long)tid);

        // Process the task
        handle_request(task);
    }
}

void enqueue_task(TaskData task)
{
    printf("new task %d\n", task.fd);
    pthread_mutex_lock(&thread_mutex);
    
    // Check if the task queue is full
    if (task_count == MAX_TASKS)
    {
        // Handle the case when the queue is full
        pthread_mutex_unlock(&thread_mutex);
        return;
    }
    
    // Enqueue the task at the rear of the queue
    queue_rear = (queue_rear + 1) % MAX_TASKS;
    task_queue[queue_rear] = task;
    task_count++;

    pthread_cond_signal(&task_cond);
    pthread_mutex_unlock(&thread_mutex);
}

TaskData dequeue_task()
{
    pthread_mutex_lock(&thread_mutex);
    TaskData task;
    
    // Check if the task queue is empty
    if (task_count == 0)
    {
        // Handle the case when the queue is empty
        // For example, you can choose to wait until a task becomes available
        pthread_mutex_unlock(&thread_mutex);
        return task;
    }
    
    // Dequeue the task from the front of the queue
    task = task_queue[queue_front];
    queue_front = (queue_front + 1) % MAX_TASKS;
    task_count--;

    pthread_mutex_unlock(&thread_mutex);
    return task;
}


char* get_client_address(struct sockaddr_storage clientaddr, socklen_t clientlen)
{
    char hostname[MAXLINE], port[MAXLINE];
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
    char *address = malloc(strlen(hostname) + strlen(port) + 2);
    sprintf(address, "%s:%s", hostname, port);
    return address;
}

void handle_request(TaskData task)
{
    printf("Handling request...\n");
    int fd = task.fd;
    struct sockaddr_storage clientaddr = task.clientaddr;
    socklen_t clientlen = task.clientlen;

    // Perform the necessary operations on the connection
    printf("Handling request from client %s\n", get_client_address(clientaddr, clientlen));
    printf("File descriptor: %d\n", fd);

    // Call the `doit` function to process the request
    doit(fd);

    // Close the connection
    Close(fd);

    printf("Request handled and connection closed.\n");
}



/*
 * doit - handle one HTTP request/response transaction
 */
/* $begin doit */
void doit(int fd) 
{
    int is_static;
    struct stat sbuf;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t rio;

    /* Read request line and headers */
    Rio_readinitb(&rio, fd);
    if (!Rio_readlineb(&rio, buf, MAXLINE))  //line:netp:doit:readrequest
        return;
    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, uri, version);       //line:netp:doit:parserequest
    if (strcasecmp(method, "GET")) {                     //line:netp:doit:beginrequesterr
        clienterror(fd, method, "501", "Not Implemented",
                    "Tiny does not implement this method");
        return;
    }                                                    //line:netp:doit:endrequesterr
    read_requesthdrs(&rio);                              //line:netp:doit:readrequesthdrs

    /* Parse URI from GET request */
    is_static = parse_uri(uri, filename, cgiargs);       //line:netp:doit:staticcheck
    if (stat(filename, &sbuf) < 0) {                     //line:netp:doit:beginnotfound
	clienterror(fd, filename, "404", "Not found",
		    "Tiny couldn't find this file");
	return;
    }                                                    //line:netp:doit:endnotfound

    if (is_static) { /* Serve static content */          
	if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) { //line:netp:doit:readable
	    clienterror(fd, filename, "403", "Forbidden",
			"Tiny couldn't read the file");
	    return;
	}
	serve_static(fd, filename, sbuf.st_size);        //line:netp:doit:servestatic
    }
    else { /* Serve dynamic content */
	if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) { //line:netp:doit:executable
	    clienterror(fd, filename, "403", "Forbidden",
			"Tiny couldn't run the CGI program");
	    return;
	}
	serve_dynamic(fd, filename, cgiargs);            //line:netp:doit:servedynamic
    }
}
/* $end doit */

/*
 * read_requesthdrs - read HTTP request headers
 */
/* $begin read_requesthdrs */
void read_requesthdrs(rio_t *rp) 
{
    char buf[MAXLINE];

    Rio_readlineb(rp, buf, MAXLINE);
    printf("%s", buf);
    while(strcmp(buf, "\r\n")) {          //line:netp:readhdrs:checkterm
	Rio_readlineb(rp, buf, MAXLINE);
	printf("%s", buf);
    }
    return;
}
/* $end read_requesthdrs */

/*
 * parse_uri - parse URI into filename and CGI args
 *             return 0 if dynamic content, 1 if static
 */
/* $begin parse_uri */
int parse_uri(char *uri, char *filename, char *cgiargs) 
{
    char *ptr;

    if (!strstr(uri, "cgi-bin")) {  /* Static content */ //line:netp:parseuri:isstatic
	strcpy(cgiargs, "");                             //line:netp:parseuri:clearcgi
	strcpy(filename, ".");                           //line:netp:parseuri:beginconvert1
	strcat(filename, uri);                           //line:netp:parseuri:endconvert1
	if (uri[strlen(uri)-1] == '/')                   //line:netp:parseuri:slashcheck
	    strcat(filename, "home.html");               //line:netp:parseuri:appenddefault
	return 1;
    }
    else {  /* Dynamic content */                        //line:netp:parseuri:isdynamic
	ptr = index(uri, '?');                           //line:netp:parseuri:beginextract
	if (ptr) {
	    strcpy(cgiargs, ptr+1);
	    *ptr = '\0';
	}
	else 
	    strcpy(cgiargs, "");                         //line:netp:parseuri:endextract
	strcpy(filename, ".");                           //line:netp:parseuri:beginconvert2
	strcat(filename, uri);                           //line:netp:parseuri:endconvert2
	return 0;
    }
}
/* $end parse_uri */

/*
 * serve_static - copy a file back to the client 
 */
/* $begin serve_static */
void serve_static(int fd, char *filename, int filesize)
{
    int srcfd;
    char *srcp, filetype[MAXLINE], buf[MAXBUF];

    /* Send response headers to client */
    get_filetype(filename, filetype);    //line:netp:servestatic:getfiletype
    sprintf(buf, "HTTP/1.0 200 OK\r\n"); //line:netp:servestatic:beginserve
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n", filesize);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: %s\r\n\r\n", filetype);
    Rio_writen(fd, buf, strlen(buf));    //line:netp:servestatic:endserve

    /* Send response body to client */
    srcfd = Open(filename, O_RDONLY, 0); //line:netp:servestatic:open
    srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0); //line:netp:servestatic:mmap
    Close(srcfd);                       //line:netp:servestatic:close
    Rio_writen(fd, srcp, filesize);     //line:netp:servestatic:write
    Munmap(srcp, filesize);             //line:netp:servestatic:munmap
}

/*
 * get_filetype - derive file type from file name
 */
void get_filetype(char *filename, char *filetype) 
{
    if (strstr(filename, ".html"))
	strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif"))
	strcpy(filetype, "image/gif");
    else if (strstr(filename, ".png"))
	strcpy(filetype, "image/png");
    else if (strstr(filename, ".jpg"))
	strcpy(filetype, "image/jpeg");
    else
	strcpy(filetype, "text/plain");
}  
/* $end serve_static */

/*
 * serve_dynamic - run a CGI program on behalf of the client
 */
/* $begin serve_dynamic */
void serve_dynamic(int fd, char *filename, char *cgiargs) 
{
    char buf[MAXLINE], *emptylist[] = { NULL };

    /* Return first part of HTTP response */
    sprintf(buf, "HTTP/1.0 200 OK\r\n"); 
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));
  
    if (Fork() == 0) { /* Child */ //line:netp:servedynamic:fork
	/* Real server would set all CGI vars here */
	setenv("QUERY_STRING", cgiargs, 1); //line:netp:servedynamic:setenv
	Dup2(fd, STDOUT_FILENO);         /* Redirect stdout to client */ //line:netp:servedynamic:dup2
	Execve(filename, emptylist, environ); /* Run CGI program */ //line:netp:servedynamic:execve
    }
    Wait(NULL); /* Parent waits for and reaps child */ //line:netp:servedynamic:wait
}
/* $end serve_dynamic */

/*
 * clienterror - returns an error message to the client
 */
/* $begin clienterror */
void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg) 
{
    char buf[MAXLINE];

    /* Print the HTTP response headers */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n\r\n");
    Rio_writen(fd, buf, strlen(buf));

    /* Print the HTTP response body */
    sprintf(buf, "<html><title>Tiny Error</title>");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<body bgcolor=""ffffff"">\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "%s: %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<p>%s: %s\r\n", longmsg, cause);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<hr><em>The Tiny Web server</em>\r\n");
    Rio_writen(fd, buf, strlen(buf));
}
/* $end clienterror */
