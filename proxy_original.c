#include <stdio.h>
#include "csapp.h"
/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";
static const char *conn_hdr = "Connection: close\r\n";
static const char *proxy_conn_hdr = "Proxy-Connection: close\r\n";
     

void at_proxy(int fd);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
int parse_uri(char *uri, char *host, char *port, char *path);
void make_requesthdrs(rio_t *rp, char *header, char *host, char *path);

int main(int argc, char **argv) {
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

    /* Check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);  // line:netp:tiny:accept
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        at_proxy(connfd);   // line:netp:tiny:doit
        Close(connfd);  // line:netp:tiny:close
    }
//   printf("%s", user_agent_hdr);
    return 0;
}


void at_proxy(int fd) {
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char host[MAXLINE], port[MAXLINE], path[MAXLINE], header[MAXLINE];

    rio_t rio;
    Rio_readinitb(&rio, fd);
    Rio_readlineb(&rio, buf, MAXLINE);
    printf("Request headers:\n");
    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, uri, version);
    if (strcasecmp(method, "GET")) {
        clienterror(fd, method, "501", "Not implemented",
                "Tiny does not implement this method");
        return; 
    }
    if (!strlen(uri) || !strlen(version)) {
        printf("Invalid request.\n");
        return;
    }

    // URI를 host, path, port로 분리 
    if(parse_uri(uri, host, port, path)){
        printf("Invalid request.\n");
        return;   
    }

    printf("checked %s, %s, %s\n", host, port, path);
    make_requesthdrs(&rio, header, host, path);
    printf("Result Header\n%s\n", header);
}


void make_requesthdrs(rio_t *rp, char *header, char *host, char *path) {
    char buf[MAXLINE], temp[MAXLINE], hosthdr[MAXLINE];
    sprintf(temp, "GET %s HTTP/1.0\r\n", path);
    Rio_readlineb(rp, buf, MAXLINE);
    while(strcmp(buf, "\r\n")) {
        if(!strncmp(buf, "Host:", 5)){
            strcat(hosthdr, buf);
            Rio_readlineb(rp, buf, MAXLINE);    
            continue;
        }
        if(strncmp(buf, "Connection:", 11) && strncmp(buf, "Proxy-Connection:", 17) && strncmp(buf, "User-Agent:", 11)){
            strcat(temp, buf);
        }
        Rio_readlineb(rp, buf, MAXLINE);    
    }
    if(strlen(hosthdr) == 0){
        sprintf(hosthdr, "Host: %s\r\n", host);
    }
    sprintf(header, "%s%s%s%s%s\r\n\r\n", temp, hosthdr, user_agent_hdr, conn_hdr, proxy_conn_hdr);    
    return;
}


int parse_uri(char *uri, char *host, char *port, char *path){
    char *prefix = "http://";
    int prelen = strlen(prefix);
    // if not http protocal, error
    if (strncmp(uri, prefix, prelen) != 0)
        return -1;

    char *start, *end;
    start = uri + prelen;
    end = start;

    // copy host
    while (*end != ':' && *end != '/') {
        end++;
    }
    strncpy(host, start, end-start);

    // port is provided
    if (*end == ':') {
        // skip ':'
        ++end;
        start = end;
        // copy port
        while (*end != '/')
        end++;
        strncpy(port, start, end-start);
    } else {
        // port is not provided, defualt 80
        strncpy(port, "80", 2);
    }

    // copy path
    strcpy(path, end);
    return 0;
}


void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg){
    char buf[MAXLINE], body[MAXBUF];
    
    /* Build the HTTP response body */
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);
    
    /* Print the HTTP response */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}