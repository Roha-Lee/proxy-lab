/*
 * proxy.c
 *
 */
#include <stdio.h>
#include "csapp.h"
/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define CACHE_OBJS_COUNT 10 
#define LRU_MAGIC_NUMBER 9999

/* You won't lose style points for including this long line in your code */
// HTTP header를 만들기 위한 상수들 
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *conn_hdr = "Connection: close\r\n";
static const char *prox_hdr = "Proxy-Connection: close\r\n";
static const char *host_hdr_format = "Host: %s\r\n";
static const char *requestlint_hdr_format = "GET %s HTTP/1.0\r\n";
static const char *endof_hdr = "\r\n";

static const char *connection_key = "Connection";
static const char *user_agent_key= "User-Agent";
static const char *proxy_connection_key = "Proxy-Connection";
static const char *host_key = "Host";

void doit(int connfd);
void parse_uri(char *uri,char *hostname,char *path,int *port);
void build_http_header(char *http_header,char *hostname,char *path,int port,rio_t *client_rio);
int connect_endServer(char *hostname,int port);
void *thread(void *vargp);
void cache_LRU(int index);
void cache_init();
int cache_find(char *url);
void readerPre(int i);
void readerAfter(int i);
void writePre(int i);
void writeAfter(int i);
int cache_eviction();


// 하나의 url에 대한 정보를 담기 위한 cache block 구조체
typedef struct {
    char cache_obj[MAX_OBJECT_SIZE];
    char cache_url[MAXLINE];
    int LRU;
    int isEmpty;

    int readCnt;            /*count of readers*/
    sem_t wmutex;           /*protects accesses to cache*/
    sem_t rdcntmutex;       /*protects accesses to readcnt*/

    int writeCnt;
    sem_t wtcntMutex;
    sem_t queue;

} cache_block;

// 캐시를 표현하는 구조체 
typedef struct {
    cache_block cacheobjs[CACHE_OBJS_COUNT];  /*ten cache blocks*/
} Cache;

// 캐시 
Cache cache;

// 캐시 초기화
void cache_init(){
    int i;
    // 캐시 구조체 초기화 
    for(i=0;i<CACHE_OBJS_COUNT;i++){
        cache.cacheobjs[i].LRU = 0;
        cache.cacheobjs[i].isEmpty = 1;
        // write를 관리하는 mutex
        Sem_init(&cache.cacheobjs[i].wmutex,0,1);
        // read count의 접근을 관리하는 mutex
        Sem_init(&cache.cacheobjs[i].rdcntmutex,0,1);
        cache.cacheobjs[i].readCnt = 0;

        cache.cacheobjs[i].writeCnt = 0;
        // write count 접근을 관리하는 mutex
        Sem_init(&cache.cacheobjs[i].wtcntMutex,0,1);
        // read/write를 요청이 온 순서대로 처리하기 위한 mutex
        Sem_init(&cache.cacheobjs[i].queue,0,1);
    }
}

// 캐시 데이터를 읽기 전에 실행하는 함수
void readerPre(int i){
    // queue를 잠가서 다른 요청을 대기하게 만든다. 
    P(&cache.cacheobjs[i].queue);
    // read count를 잠그고 critical section 진입
    P(&cache.cacheobjs[i].rdcntmutex);
    cache.cacheobjs[i].readCnt++;
    // Readcount가 1개 이상이면 write를 잠금
    if(cache.cacheobjs[i].readCnt==1) P(&cache.cacheobjs[i].wmutex);
    // readCnt에 대한 critical section 나왔으므로 unlock
    V(&cache.cacheobjs[i].rdcntmutex);
    // queue를 unlock하여 다른 스레드의 요청 허용 
    V(&cache.cacheobjs[i].queue);
}

// 캐시 데이터를 읽고나서 실행하는 함수
void readerAfter(int i){
    // read count를 다루기 전에 lock
    P(&cache.cacheobjs[i].rdcntmutex);
    // read count를 1감소시키고 0이 되면 write가능하도록 Lock 해제
    cache.cacheobjs[i].readCnt--;
    if(cache.cacheobjs[i].readCnt==0) V(&cache.cacheobjs[i].wmutex);
    // read count에 대한 잠금 해제 
    V(&cache.cacheobjs[i].rdcntmutex);
}

// 캐시 데이터를 쓰기 전에 실행하는 함수 
void writePre(int i){
    P(&cache.cacheobjs[i].wtcntMutex);
    cache.cacheobjs[i].writeCnt++;
    if(cache.cacheobjs[i].writeCnt==1) P(&cache.cacheobjs[i].queue);
    V(&cache.cacheobjs[i].wtcntMutex);
    P(&cache.cacheobjs[i].wmutex);
}

// 캐시 데이터를 쓰고나서 실행하는 함수 
void writeAfter(int i){
    V(&cache.cacheobjs[i].wmutex);
    P(&cache.cacheobjs[i].wtcntMutex);
    cache.cacheobjs[i].writeCnt--;
    if(cache.cacheobjs[i].writeCnt==0) V(&cache.cacheobjs[i].queue);
    V(&cache.cacheobjs[i].wtcntMutex);
}


// 동일한 url을 가진 캐시 데이터가 있는지 확인 
int cache_find(char *url){
    int i;
    for(i=0;i<CACHE_OBJS_COUNT;i++){
        // LOCK
        readerPre(i);
        // cache에 url에 대응하는 정보가 있으면 break
        if((cache.cacheobjs[i].isEmpty==0) && (strcmp(url,cache.cacheobjs[i].cache_url)==0)){
            // UNLOCK
            readerAfter(i);
            break;
        }
        // UNLOCK
        readerAfter(i);
    }
    // 캐시를 찾을 수 없는 경우 -1 반환
    if(i>=CACHE_OBJS_COUNT) return -1; 
    // 캐시 인덱스 반환
    return i;
}

/*find the empty cacheObj or which cacheObj should be evictioned*/
int cache_eviction(){
    int min = LRU_MAGIC_NUMBER;
    int minindex = 0;
    int i;
    for(i=0; i<CACHE_OBJS_COUNT; i++)
    {
        readerPre(i);
        if(cache.cacheobjs[i].isEmpty == 1){/*choose if cache block empty */
            minindex = i;
            readerAfter(i);
            break;
        }
        if(cache.cacheobjs[i].LRU < min){    /*if not empty choose the min LRU*/
            minindex = i;
            readerAfter(i);
            continue;
        }
        readerAfter(i);
    }

    return minindex;
}

/*update the LRU number except the new cache one*/
void cache_LRU(int index){

    writePre(index);
    cache.cacheobjs[index].LRU = LRU_MAGIC_NUMBER;
    writeAfter(index);

    int i;
    for(i=0; i<CACHE_OBJS_COUNT; i++)    {
        if (i == index) continue;

        writePre(i);
        if(cache.cacheobjs[i].isEmpty==0 && i!=index){
            cache.cacheobjs[i].LRU--;
        }
        writeAfter(i);
    }
}

/*cache the uri and content in cache*/
void cache_uri(char *uri,char *buf){
    int i = cache_eviction();

    writePre(i);/*writer P*/

    strcpy(cache.cacheobjs[i].cache_obj,buf);
    strcpy(cache.cacheobjs[i].cache_url,uri);
    cache.cacheobjs[i].isEmpty = 0;

    writeAfter(i);/*writer V*/

    cache_LRU(i);
}


/*
  thread를 통해 처리하려고 하는 루틴을 만든다. 
*/ 
void *thread(void *vargp){
  // vargp를 int형 포인터로 바꾸고 dereference한다. 
    int connfd = *((int *)vargp);
    // Thread를 detach로 바꾸어 준다. -> 커널이 자동으로 종료되면 거둬간다. 
    Pthread_detach(pthread_self());
    // 동적할당해 주었던 vargp를 해제해준다. 
    Free(vargp);
    // HTTP request 처리
    doit(connfd);
    // connfd를 닫아준다. 
    Close(connfd);
    return NULL;
}


/*
서버용 소켓을 만들고 
client에서 요청이 들어오면 
Accept을 하여 connfd를 만들어 준 후 
doit에서 요청을 처리한다. 
*/
int main(int argc,char **argv)
{
    int listenfd, *connfdp;
    socklen_t  clientlen;
    // thread id
    pthread_t tid;
    char hostname[MAXLINE],port[MAXLINE];
    struct sockaddr_storage clientaddr;
    // 캐시 초기화 
    cache_init();
    // port를 입력하지 않은 경우는 에러메시지를 출력하고 프로그램 종료 
    if(argc != 2){
        fprintf(stderr,"usage :%s <port> \n",argv[0]);
        exit(1);
    }
    // 서버용 듣기 소켓을 만들어 준다. socket -> bind -> listen 
    Signal(SIGPIPE, SIG_IGN);
    listenfd = Open_listenfd(argv[1]);
    while(1){
        clientlen = sizeof(clientaddr);
        // 동적할당으로 connfd를 생성한다. 
        connfdp = Malloc(sizeof(int));
        // Accept를 통해 connection을 생성하고 fd를 connfdp가 가리키는 값으로 넣어준다. 
        *connfdp = Accept(listenfd,(SA *)&clientaddr,&clientlen);
        // connfdp를 thread함수의 인자로 넘겨준다. 
        Pthread_create(&tid,NULL,thread, connfdp);
    }
    return 0;
}


/*
client에서 요청한 http request를 처리하는 함수 
*/
void doit(int connfd)
{
    int end_serverfd;/*the end server file descriptor*/

    char buf[MAXLINE],method[MAXLINE],uri[MAXLINE],version[MAXLINE];
    char endserver_http_header [MAXLINE];
    /*store the request line arguments*/
    char hostname[MAXLINE],path[MAXLINE];
    int port;

    // client용 rio -> rio
    // end server용 rio -> server_rio
    rio_t rio, server_rio;

    // client와 connection하기 위한 rio를 파일식별자와 묶어서 초기화 해준다. 
    Rio_readinitb(&rio,connfd);
    // client에서 전송된 HTTP 요청을 읽어서 method/uri/version으로 나누어 넣어준다. 
    Rio_readlineb(&rio,buf,MAXLINE);
    sscanf(buf,"%s %s %s",method,uri,version);

    // GET에 대해서만 처리하기로 했으므로 GET이 아닌 경우 에러 메시지를 출력한다. 
    if(strcasecmp(method,"GET")){
        printf("Proxy does not implement the method");
        return;
    }

    char url_store[100];
    strcpy(url_store,uri);
    
    int cache_index;
    if((cache_index=cache_find(url_store))!=-1){ 
    /*in cache then return the cache content*/
        readerPre(cache_index);
        Rio_writen(connfd,cache.cacheobjs[cache_index].cache_obj,
                    strlen(cache.cacheobjs[cache_index].cache_obj));
        readerAfter(cache_index);
        cache_LRU(cache_index);
        return;
    }
    // URI를 파싱하여 hostname, path, port 정보를 얻어낸다. 
    parse_uri(uri,hostname,path,&port);

    // URI에서 얻은 정보와 전역변수에서 정의한 상수들을 이용하여 end server에서 request를 보낼 HTTP header를 만든다. 
    build_http_header(endserver_http_header,hostname,path,port,&rio);
    printf("ROHA %s\n", endserver_http_header);
    // end server와 연결하고 새로운 소켓에 대한 fd를 얻는다. 
    end_serverfd = connect_endServer(hostname,port);
    if(end_serverfd<0){
        printf("connection failed\n");
        return;
    }

    // end server와 read/write를 하기 위해 server_rio와 server fd를 묶어준다. 
    Rio_readinitb(&server_rio, end_serverfd);
    
    // end server에게 HTTP request를 보낸다. 
    Rio_writen(end_serverfd,endserver_http_header,strlen(endserver_http_header));

    /*store it*/
    char cachebuf[MAX_OBJECT_SIZE];
    int sizebuf = 0;
    size_t n;
    // end server로 부터 돌아온 response를 읽어서 값이 있으면 client에게 그대로 전달해준다. 
    while((n=Rio_readlineb(&server_rio,buf,MAXLINE))!=0)
    {
        sizebuf += n;
        if(sizebuf < MAX_OBJECT_SIZE){
            strcat(cachebuf, buf);
        }
        // printf("proxy received %d bytes,then send\n",n);
        Rio_writen(connfd,buf,n);
    }
    
    if(sizebuf < MAX_OBJECT_SIZE){
        cache_uri(url_store,cachebuf);
    }

    // end server의 소켓을 닫아준다. 
    Close(end_serverfd);
}


/*
    클라이언트에서 받은 정보들을 가지고 end server에게 보낼 HTTP request header를 만든다.
*/
void build_http_header(char *http_header,
        char *hostname,char *path,int port,rio_t *client_rio)
{
    char buf[MAXLINE],request_hdr[MAXLINE],other_hdr[MAXLINE],host_hdr[MAXLINE];
    // end server에게 요청할 파일의 경로가 path에 있으므로 
    // GET path VERSION형식의 request 헤더를 작성한다. 
    sprintf(request_hdr,requestlint_hdr_format,path);
    
    // client에서 전송된 데이터를 읽으면서 다음을 수행
    while(Rio_readlineb(client_rio,buf,MAXLINE)>0)
    {
        // endof_hdr 즉 \r\n\r\n이 오면 EOF이므로 break를 통해 while문을 탈출한다. 
        if(strcmp(buf,endof_hdr)==0) break;
        // client에서 보낸 HTTP header에 host정보가 있으면 그대로 host_hdr에 복사한다. 
        // 없는 경우는 아래에서 Host: hostname(URI에서 얻은 정보)의 형태로 만들어준다.
        if(!strncasecmp(buf,host_key,strlen(host_key)))/*Host:*/
        {
            strcpy(host_hdr,buf);
            continue;
        }
        // Connection: close
        // Proxy-Connection: close
        // User-Agent: Mozilla/5.0 ~~~
        // 를 제외한 나머지 헤더 내용은 그대로 other_hdr에 더해준다. 
        if(strncasecmp(buf, connection_key, strlen(connection_key))
         &&strncasecmp(buf, proxy_connection_key, strlen(proxy_connection_key))
         &&strncasecmp(buf, user_agent_key, strlen(user_agent_key)))
        {
            strcat(other_hdr, buf);
        }
    }

    // client에서 보낸 HTTP request에 Host: ~가 없는 경우 만들어 준다. 
    if(strlen(host_hdr)==0)
    {
        sprintf(host_hdr,host_hdr_format,hostname);
    }
    // 모두 합쳐서 최종 header를 만들어준다. 
    sprintf(http_header,"%s%s%s%s%s%s%s",
            request_hdr,
            host_hdr,
            conn_hdr,
            prox_hdr,
            user_agent_hdr,
            other_hdr,
            endof_hdr);

    return ;
}

// end server와 통신을 하기 위한 client 소켓을 만들어준다. 
inline int connect_endServer(char *hostname,int port){
    char portStr[100];
    sprintf(portStr,"%d",port);
    return Open_clientfd(hostname,portStr);
}

// uri를 파싱하여 Hostname, path, port 정보를 얻는다. 
void parse_uri(char *uri,char *hostname,char *path,int *port)
{

    *port = 80;
    // "http://"의 더블슬래시 부분을 찾아서 위치를 반환한다. 
    char* pos = strstr(uri,"//");
    // "//"가 없으면 uri를 pos에 넣어주고 있다면 pos+2를 (hostname의 시작부분)넣어준다. 
    pos = pos!=NULL? pos+2:uri;
    // port정보를 얻기위해 ":"을 찾아서 위치를 pos2에 넣는다. 
    char*pos2 = strstr(pos,":");
    // 포트정보가 있다면 
    if(pos2!=NULL)
    {
        // colon부분을 null문자로 바꾸어서 pos부터 읽었을때 colon위치를 스트링의 끝으로 인식하게 하여 hostname부분을 파싱한다. 
        // 즉, //뒤부터 :앞까지를 읽어서 hostname에 넣어준다. 
        *pos2 = '\0';
        sscanf(pos,"%s",hostname);
        // 포트부분과 경로부분을 읽어서 넣어준다. 
        sscanf(pos2+1,"%d%s",port,path);
    }
    // 포트 정보가 없다면 
    else
    {
        // "/"부분을 찾아서 있다면 
        pos2 = strstr(pos,"/");
        if(pos2!=NULL)
        {
            // "/"이전부분이 hostname, 이후부분이 path이므로 "/" 자리에 NULL을 넣은 후 
            // 앞에서 부터 읽어서 hostname정보를 얻어내고 
            *pos2 = '\0';
            sscanf(pos,"%s",hostname);
            // 다시 "/"를 넣어준 후 pos2위치부터 읽어서 경로를 얻어낸다. 
            *pos2 = '/';
            sscanf(pos2,"%s",path);
        }
        // "/"부분을 찾아서 없다면 
        else
        {
            // 경로부분, 포트부분이 없는 것이므로 hostname만 넣어준다. 
            sscanf(pos,"%s",hostname);
        }
    }
    return;
}