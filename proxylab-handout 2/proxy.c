/*
 * proxy.c - A web proxy supporting concurrent requests and caching:
 * Our proxy, implemented in this lab, transimits the requests from
 * the client to servers and then forward the responses from servers
 * to clients. And our proxy have two major features. The first feature
 * is that it has a small cache. When receiving a request from a client,
 * it checks whether it is in the cache at first. If it is, the proxy gets
 * the object from cache and than returns it to the client. Otherwise,
 * the proxy sends the request to the servers and  when gettig the
 * repsonse from servers, store the response in the cache, then send it
 * to the client. The proxy also uses multi-threads to support concurrent
 * requests from multiple clients.
 *
 * Andrew ID 1: dil1
 * Name 1: Di Li
 * Andrew ID 2: mingleic
 * Name 2: Minglei Chen
 */

#include <stdio.h>
#include "csapp.h"
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400


/* Header information */
static const char *user_agent = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *accept_msg = "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n";
static const char *accept_encoding = "Accept-Encoding: gzip, deflate\r\n";
static const char *connection = "Connection: close\r\n";
static const char *proxy_connection = "Proxy-Connection: close\r\n";

/* Error message*/ 
static char *port_error = "Invalid port number!\n"; 
static char *host_error = "Invalid uri!\n"; 
static char *conn_error = "Connection error!\n"; 
static char *GET_kind_msg = "Sorry, we only support GET method.\n";
static char *invalid_HTTP_msg = "Sorry, invalid HTTP version.\n";
static char *invalid_uri_msg = "Sorry, invalid uri.\n";

static sem_t mutex; /* Semaphore used for gethostname func*/
sem_t sem_total_cache_size; /* Semaphore used for update the total size of cache*/
pthread_rwlock_t rwlock; /* Read and write lock for cache list*/
/* Structure for a line in cahce*/
struct line
{
    int valid;
    char* tag;
    unsigned int len;
    char* block;
    struct line* ptr;
};
/*Header for cache list. my_line points to the first line. Our cache
 * has one set and several lines. The cache is implemented
 * in a single linked list and each list node represents
 * a line in the cache.*/
struct line* my_line = NULL;
unsigned int Total_cache_size = 0; /* Total cahe size*/

struct line* get_cache_data(char* uri,char* buf);
void set_cache_data(char* uri,char* buf,unsigned int datasize);
void Adapt_LRU(struct line* sign);
void doit(int fd);
void *thread(void *vargp);
int check_request(int fd,char* method, char* uri, char* version);
int parse_uri(char* uri, char* host, char* path, int port);
char *parse_header(char *header);
int open_clientfd_multi(char *hostname, int port);
void sigint_handler(int sig);


int main(int argc, char **argv)
{
    int i, listenfd, *connfd;
    int port;
    struct sockaddr_in clientaddr;
    socklen_t clientlen = sizeof(struct sockaddr_in); 
    pthread_t tid;
    /* Check command line arguments*/
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(0);
    }
    if (strlen(argv[1]) > 5) {
        fprintf(stderr, "Error: port invalid\n");
        exit(0);
    }
    for (i = 0; i < strlen(argv[1]); i++) {
        if (argv[1][i] > 57 || argv[1][i] < 48) {
            fprintf(stderr, "Error: port invalid\n");
            exit(0);
        }
    }
    /* Get the port */
    port = atoi(argv[1]);
    if (port > 65535 || port < 0) {
        fprintf(stderr, "Error: port invalid\n");
        exit(0);
    }
    Signal(SIGPIPE, SIG_IGN); /* Ignore sigpipe*/
    Signal(SIGINT, sigint_handler);
    listenfd = Open_listenfd(port); /* Open listening fd*/
    /* Initialization for semaphores and lock*/
    pthread_rwlock_init(&rwlock,NULL);
    Sem_init(&mutex, 0, 1);
    Sem_init(&sem_total_cache_size,0,1);

    while (1) {
        connfd = malloc(sizeof(int));
        *connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        /* Create new thread for each request*/
        if (pthread_create(&tid, NULL, thread, connfd) < 0) {
            fprintf(stderr, "thread create error.\n");
        }
    }
    return 0;
}

/* thread: Thread routine*/
void *thread(void *vargp)
{
    int connfd = *((int *)vargp);
    if (pthread_detach(pthread_self()) < 0) {
        fprintf(stderr, "thread detach error.\n");
        pthread_exit("thread detach error");
    }
    free(vargp);
    doit(connfd);
    close(connfd);
    return NULL;
}

/*
 * doit: Handle one HTTP request and get response from cache or server,
 * and return the response to client.
 */
void doit(int fd)
{
    int n = 0;
    int clientfd;
    int port = 80;
    char host[MAXLINE] = {0}, path[MAXLINE] = {0};
    char method[MAXLINE] = {0}, uri[MAXLINE] = {0}; 
    char version[MAXLINE] = {0}, header[MAX_OBJECT_SIZE] = {0};
    char buf1[MAX_OBJECT_SIZE] = {0}, buf2[MAX_OBJECT_SIZE] = {0};
    rio_t rio1, rio2;
    unsigned int datasize = 0;
    struct line* sign;
    /* Reset buffers*/
    memset(header, 0, sizeof(header));
    memset(buf1, 0, sizeof(buf1));
    memset(buf2, 0, sizeof(buf2));
    memset(host, 0, sizeof(host));
    memset(path, 0, sizeof(path));
    memset(method, 0, sizeof(method));
    memset(uri, 0, sizeof(uri));
    memset(version, 0, sizeof(version));
    /* Read request line*/
    rio_readinitb(&rio1, fd);
    if ((n = rio_readlineb(&rio1, buf1, MAXLINE)) <= 0) {
         if (n < 0) 
            fprintf(stderr,"rio_readlieb error: read request line failed!\n");
         return;
    }
    sscanf(buf1, "%s %s %s", method, uri, version);
    /*Call function to check whether request is qualified */ 
    if (check_request(fd,method, uri, version) < 0) return;
    /*Call function to get hostname and path from uri */
    if (parse_uri(uri, host, path, port) < 0) {
        rio_writen(fd, port_error, strlen(port_error)); 
        return;
    }    
    memset(buf1, 0, sizeof(buf1));   
    /* Find response in cache and add read lock here*/
    pthread_rwlock_rdlock(&rwlock);
    sign = get_cache_data(uri,buf1);
    pthread_rwlock_unlock(&rwlock);
    /* If in cache*/
    if(sign != NULL) {
        printf("Get content from cache !!\n");
        /* Send response in cache to client*/
        if ((n = rio_writen(fd,buf1,sizeof(buf1))) < 0) {
            fprintf(stderr, \
                "rio_writen error: send data to client from cache failed!\n");
            return;
        }
        memset(buf1,0,sizeof(buf1));
        /* Update the object position in cache*/
        pthread_rwlock_wrlock(&rwlock);
        Adapt_LRU(sign);
        pthread_rwlock_unlock(&rwlock);
        return;
    }
    /* If not in cache*/
    else {
        printf("Get content from real server!\n");
        /* Read request headers*/
    	while ((n = rio_readlineb(&rio1, buf1, MAXLINE)) > 0) { 
            if (!strcmp(buf1, "\r\n")) break;
            sprintf(header, "%s%s", header, parse_header(buf1));
            memset(buf1, 0, sizeof(buf1));
   	}
        if (n < 0) {
	    fprintf(stderr, \
		"rio_read error: read request from client failed!\n");
            return;
        }
    	memset(buf1, 0, sizeof(buf1));
        /* Connect to servers*/
    	if ((clientfd = open_clientfd_multi(host, port)) < 0) {
            fprintf(stderr, "Connect server error!\n");
            if (clientfd == -1) 
		rio_writen(fd, conn_error, strlen(conn_error));
            else if (clientfd == -2)
		rio_writen(fd, host_error, strlen(host_error));
            close(clientfd);
            return;
    	}
    	rio_readinitb(&rio2, clientfd);
        /* Send request to severs*/
    	sprintf(buf1, "GET %s HTTP/1.0\r\nHost: %s\r\n%s%s%s", \
		path, host, user_agent, accept_msg, accept_encoding);
    	sprintf(buf1, "%s%s%s%s\r\n", \
		buf1, connection, proxy_connection, header);
    	printf("buf1:\n%s", buf1);
    	if (rio_writen(clientfd, buf1, strlen(buf1)) < 0) {
            fprintf(stderr, "rio_writen error: fail to send request!\n");
            close(clientfd);
            return;
    	}
    	memset(buf1, 0, sizeof(buf1));
        /* Read response from servers*/
        while ((n = rio_readnb(&rio2, buf2, MAX_OBJECT_SIZE)) > 0) {
            /*If this can be saved in cache, we copy content to buf1 */
            if (n < MAX_OBJECT_SIZE) {
                datasize += n;
                memcpy(buf1,buf2,n);
                if (rio_writen(fd, buf2, n) < 0) {
                    fprintf(stderr, \
			"rio_writen error: fail to send response!\n");
                    close(clientfd);
                    return; 
                }
                memset(buf2, 0, sizeof(buf2));
            }
            /*If this cannot be cached , we simply write to fd */ 
            else {      
                datasize += n;
                if (rio_writen(fd, buf2, n) < 0) {
                     fprintf(stderr, \
			 "rio_writen error: fail to send response!\n");
                     close(clientfd);
                     return; 
           	}
            	memset(buf2, 0, sizeof(buf2));
            }
        }
        if (n < 0) {
            fprintf(stderr, \
		"rio_readnb error: faile to read response from server!\n");
            close(clientfd);
            return;
        }
        /* Check the size of the response*/
        if (datasize < MAX_OBJECT_SIZE) {
            printf("Enter to write cache\n");
            /* Write the response to cache*/
            pthread_rwlock_wrlock(&rwlock);
            set_cache_data(uri,buf1,datasize);
            pthread_rwlock_unlock(&rwlock);
            printf("Write end\n");
        }
        /*reset the variable */
        datasize = 0;
        memset(buf1,0,sizeof(buf1));
    	close(clientfd);
    }
    printf("Connection to one client closed\n");
}
/*Check the format of request and return whether it is qualified */
int check_request(int fd,char* method, char* uri, char* version)
{
    char uri_test[7] = {0};
    /*Check GET method*/
    if (strcasecmp(method, "GET") != 0) {
        rio_writen(fd,GET_kind_msg,strlen(GET_kind_msg));
        return -1;
    }
    /*Check http version */
    if (strcasecmp(version, "HTTP/1.0") && strcasecmp(version, "HTTP/1.1")) {
        rio_writen(fd,invalid_HTTP_msg,strlen(invalid_HTTP_msg));
        return -1;
    }
    strncpy(uri_test, uri, 7);
    /*Check the hostname format */
    if (strcasecmp(uri_test, "http://")) {
        rio_writen(fd,invalid_uri_msg,strlen(invalid_uri_msg));
        return -1;
    }
    return 0;
}

/* 
 * Adapt_LRU: This is the function used to update the positions of recently
 * used object. After one cache object is read from the cache, this function 
 * will be called to put the oject in the first place in the cache list
 */
void Adapt_LRU(struct line* sign)
{
    struct line* tmp = my_line;
    if (tmp == sign) return;
    else {
        while (tmp->ptr != sign) {
            if (tmp->ptr == NULL) return; 
            tmp = tmp->ptr;
        }
        /* Put the object in the first position in cache*/
        tmp->ptr = (tmp->ptr) -> ptr;
        sign -> ptr = my_line;
        my_line = sign;
    }
}

/*
 * set_cache_data: This function is used to create new object in cache.
 * After a new reponse is received from server, if the size of the 
 * response is small than MAX_OBJECT_SIZE, the function will be called
 * to cache the new object and may delete some objects in the cache list
 * to keep the total size of cache small the the MAX_CACHE_SIZE.
 */
void set_cache_data(char* uri,char* buf,unsigned int datasize)
{
    struct line* tmp_line = malloc(sizeof(struct line));
    struct line* traverse_ptr = NULL;
    struct line* pre_ptr = NULL;
    unsigned int buf_size = datasize;
    /* set the value to each element in line */
    tmp_line -> valid = 1;
    tmp_line -> len = buf_size;
    tmp_line -> tag = malloc(strlen(uri));
    strncpy(tmp_line ->tag, uri, strlen(uri));
    tmp_line -> block = malloc(buf_size);
    memcpy(tmp_line-> block, buf, buf_size);
    tmp_line -> ptr = NULL;
    P(&sem_total_cache_size);
    /* If total size of cache is small than MAX_CACHE_SIZE,
     * just insert the object.
     */
    if (Total_cache_size + buf_size < MAX_CACHE_SIZE) {
        if (my_line == NULL)
            my_line = tmp_line;
        else {
            tmp_line -> ptr = my_line;
            my_line = tmp_line;
        }
        Total_cache_size += buf_size;
    }
    /* If the total size of cache is small than MAX_CACHE_SIZE,
     * delete the least recently used one util the size is smaller
     * than MAX_CACHE_SIZE, than inert the new one.
     */
    else {
        tmp_line -> ptr = my_line;
        my_line = tmp_line;
        Total_cache_size = Total_cache_size + buf_size;
        while(Total_cache_size > MAX_CACHE_SIZE)
        {
            traverse_ptr = my_line;
            while(traverse_ptr->ptr != NULL)
            {
                pre_ptr = traverse_ptr;
                traverse_ptr = traverse_ptr->ptr; 
            }
            pre_ptr -> ptr = NULL;
            Total_cache_size = Total_cache_size - traverse_ptr->len;
            /*free to avoid memory leak */
            free(traverse_ptr -> tag);
            free(traverse_ptr -> block);
            free(traverse_ptr);
        }
    }
    V(&sem_total_cache_size);
}

/*
 * get_cache_data: This function is used to find whether a request
 * can be found in cache. If so, the pointer to the cache object is
 * returned. Otherwise, NULL is returned.
 */
struct line* get_cache_data(char* uri,char* buf)
{
    struct line* traverse_ptr = my_line;
    while(traverse_ptr != NULL)
    {
        if((traverse_ptr->valid == 1) && (strcmp(traverse_ptr->tag,uri) == 0))
        {
            memcpy(buf,traverse_ptr->block,traverse_ptr->len);
            break;
        }
        traverse_ptr = traverse_ptr -> ptr;
    }
    return traverse_ptr;
}

/* 
 * open_clientfd_multi: This function is developed from the
 * open_clientfd function in csapp.c, but with a thread
 * safe variant mutex.\ to safe the gethostbyname funct.
 */
int open_clientfd_multi(char *hostname, int port) 
{
    int clientfd;
    struct hostent *hp;
    struct sockaddr_in serveraddr;

    if ((clientfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	return -1; /* check errno for cause of error */

    /* Fill in the server's IP address and port */
    P(&mutex);
    printf("before gethostbyname...\n");
    if ((hp = gethostbyname(hostname)) == NULL) {
        V(&mutex);
	return -2; /* check h_errno for cause of error */
    }
    printf("after gethostbyname...\n");
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)hp->h_addr_list[0], 
	  (char *)&serveraddr.sin_addr.s_addr, hp->h_length);
    serveraddr.sin_port = htons(port);
    V(&mutex);
    /* Establish a connection with the server */
    if (connect(clientfd, (SA *) &serveraddr, sizeof(serveraddr)) < 0)
	return -1;
    return clientfd;
}

/* 
 * parse_uri: This function is used to parse the uri, to get the host,
 * path, and port. The default port number is 80.
 */
int parse_uri(char* uri, char* host, char* path,  int port)
{
    int i = 0, p = 0, h = 0, p_hd = 0;
    char tmp;
    char newport[MAXLINE];
    for (i = 0, h = 0, p = 0, p_hd = 0; (tmp = *(uri+i)) != '\0'; i++) {
        if (tmp == ':' && i > 6 && !p_hd) { 
            p_hd = i;
        }
        else if (i > 6 && tmp == '/') {
            p = i;
            break;
        }
        else if (i > 6 && !p_hd) { 
            *(host + h) = *(uri + i);
            h++;
        }
    }
    /* Get the path*/
    for (i = 0; (tmp = *(uri+p)) != '\0'; p++, i++) {
        *(path + i) = tmp;
    }
    /* Decide the port number*/
    if (p_hd == 0)
        port = 80;
    else {
        for (i = 0; (tmp = *(uri + p_hd)) != '/' && tmp != '\0'; i++, p_hd++) {
            if (i > 4) return -1;
            if (tmp < 48 || tmp > 57) return -1;
            *(newport + i) = tmp;
        }
        if ((port = atoi(newport)) > 65535 || port < 0) return -1;
    }
    return 0;
}

/* 
 * parse_header: This function is used to filter the request headers.
 * If one header is in the requested header list in the handout, the
 * header will be filtered. And if not, it will be returned.
 */
char *parse_header(char *header) 
{
    char tmp;
    char tmp_header[MAXLINE] = {0};
    int i = 0, j = 0;
    /* Get the header type*/
    while ((tmp = *(header + i)) != ':' && tmp != '\0') {
        *(tmp_header + j) = tmp;
        j++;
        i++;
    }
    /*If this does not belong to any of this five kinds */
    if (strcasecmp(tmp_header, "Host") && strcasecmp(tmp_header,"User-Agent") \
            && strcasecmp(tmp_header,"Accept") \
	    && strcasecmp(tmp_header,"Accept-Encoding") \
            && strcasecmp(tmp_header,"Connection") \
            && strcasecmp(tmp_header,"Proxy-Connection"))
       return header;
    memset(header, 0, sizeof(header));
    return header;
}

void sigint_handler(int sig)
{
    pthread_rwlock_wrlock(&rwlock);
    struct line * traverse_ptr = my_line;
    struct line * next_ptr = NULL;
    printf("exit\n");
    while(traverse_ptr != NULL)
    {
        next_ptr = traverse_ptr->ptr;
        free(traverse_ptr -> tag);
        free(traverse_ptr -> block);
        free(traverse_ptr);
        traverse_ptr = next_ptr;
    }
    pthread_rwlock_unlock(&rwlock);
    exit(0);
}
