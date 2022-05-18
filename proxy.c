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
static const char *prox_hdr = "Proxy-Connection: close\r\n";
static const char *host_hdr_format = "Host: %s\r\n";
static const char *requestline_hdr_format = "GET %s HTTP/1.0\r\n";
static const char *endof_hdr = "\r\n";

static const char *host_key = "Host";
static const char *connection_key = "Connection";
static const char *proxy_connection_key = "Proxy-Connection";
static const char *user_agent_key = "User-Agent";

/* end server info */
static const char *end_server_host = "localhost";   // 현재, 한 컴퓨터에서 프록시와, 서버가 돌아가기 때문에 localhost라고 지칭
static const int end_server_port = 8000;           // tiny server의 포트 번호

void doit(int fd);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void parse_uri(char *uri, char *host, int *port, char *path);
void build_http_header(char *http_header, char *hostname, char *path, rio_t *client_rio);
int connect_endServer(char *hostname, int port);

void init_cache(void);
void *thread(void *connfdp);
int reader(int connfd, char *url);
void writer(char *url, char *buf);


/* 캐시 구조체 */
typedef struct {
  char *url; // url을 담을 변수
  int *flag; // 캐시가 비어있는지 차 있는지 구분할 변수
  int *cnt; // 최근 방문 순서를 나타내기 위한 변수
  char *content; // 클라이언트에게 보낼 내용이 담겨있는 변수
} Cache_info;

Cache_info *cache; // cache 변수 선언
int readcnt; // 세마포어를 cnt할 변수
sem_t mutex, w;


// argc = 2, argv[1] = 9094
int main(int argc, char **argv) {
    int listenfd;
    socklen_t clientlen;
    char hostname[MAXLINE], port[MAXLINE];
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    // 캐시 ON
    init_cache();
    /* Check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);  // 지정한 포트 번호로 듣기 식별자 생성
    
    while (1) {
      // server main에서 일련의 처리를 하는 대신 스레드를 분기하여
      // 각 연결에 대해 이후의 과정을 스레드 내에서 병렬적으로 처리한다.
      // main은 다시 while문의 처음으로 돌아가 새로운 연결을 기다림
      clientlen = sizeof(clientaddr);
      
      // 스레드마다 각각의 connfd를 유지하기 위해 연결할때마다 메모리를 할당하여 포인팅해줌
      int *connfdp = Malloc(sizeof(int)); // 메모리 동적할당
      *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
      
      Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
      printf("Accepted connection from (%s, %s)\n", hostname, port);

      // thread 생성
      // 연결마다 고유한 connfd를 인자로 가져감
      Pthread_create(&tid, NULL, thread, (void *)connfdp); // 프로세스 내에서 쓰레드 만들기
    }
    return 0;
}

void *thread(void *connfdp)
{
  // 각 스레드별 connfd는 입력으로 가져온 connfdp가 가리키던 할당된 위치의 fd의 값
  int connfd = *((int *)connfdp);
  Pthread_detach(pthread_self()); // 연결 가능한 쓰레드 tid 분리. pthread_self()를 인자로 넣으면 자신을 분리
  Free(connfdp); // connfdp는 이미 connfd를 얻어 역할을 다했으니 반납해줌
  
  // sequential과 같은 과정을 진행
  doit(connfd); 
  Close(connfd);
  return NULL;
}

/** 한 개의 HTTP 트랜잭션을 처리 */
void doit(int connfd) {
    int endserver_fd;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char endserver_http_header[MAXLINE]; // 서버에 보낼 http 헤더 정보를 담을 공간
    char hostname[MAXLINE], path[MAXLINE]; // hostname : IP주소를 담을 공간, path : 경로를 담을 공간
    int port;

    char url[MAXLINE];
    char content_buf[MAX_OBJECT_SIZE];

    /*
      Rio의 장점 : EOF을 만났을 때 short count가 나옴을 보장한다. (short count : 원하는만큼 읽겠다고 했는데 다 못 읽는 경우에 발생)
      rio : 클라이언트와 프록시 사이의 버퍼
      server_rio : 프록시와 서버 사이의 버퍼
    */
    rio_t rio, endserver_rio;

    Rio_readinitb(&rio, connfd); // rio와 connfd 연결
    Rio_readlineb(&rio, buf, MAXLINE); // 클라이언트가 보낸 요청라인을 읽고 분석. buf에 복사함
    sscanf(buf, "%s %s %s", method, uri, version);
    strcpy(url, uri);
        
    if (strcasecmp(method, "GET")) {   // 결과가 0이 아닐 경우임. 즉, GET method만 처리
        clienterror(connfd, method, "501", "Not implemented", "Proxy does not implement this method");
        return;
    }

    /* cache에서 찾았을 때, cache Hit */
    if (reader(connfd, url)) {
      return;
    }

    // uri를 파싱해서 hostname, path, port번호에 값 맵핑
    parse_uri(uri, hostname, &port, path); // hostname과, port는 모르는 상태로 호출
    // 실행하고 나면 hostname, port, path에 값이 매핑되어 있음

    // build http request for end server
    // 서버로 보낼 http 헤더를 생성
    build_http_header(endserver_http_header, hostname, path, &rio);

    // connect to end server
    // 서버에 연결하면서 입출력에 대해 준비된 소켓 식별자 리턴
    // 서버 입장에서는 프록시가 클라이언트임.
    endserver_fd = connect_endServer(hostname, port);
    if (endserver_fd < 0) {
      printf("connection failed\n");
      return;
    }

    Rio_readinitb(&endserver_rio, endserver_fd); // server_rio 초기화. 프록시와 서버 연결
    // 프록시에서 만든 http 헤더를 endserver로 전달
    Rio_writen(endserver_fd, endserver_http_header, strlen(endserver_http_header));
    
    size_t n; // 버퍼에다 쓴 내용의 크기
    int total_size = 0;
    while ((n = Rio_readlineb(&endserver_rio, buf, MAXLINE)) != 0) {          // end server의 응답을 buf에 받기
        printf("Proxy received %ld bytes, then send\n", n);                   // proxy에 end server에서 받은 문자수를 출력하고
        Rio_writen(connfd, buf, n);                                               // client에 end server response를 출력

        /* cache content의 최대 크기를 넘지 않으면 */
        if (total_size+n < MAX_OBJECT_SIZE) {
          strcpy(content_buf + total_size, buf);
        }
        total_size += n;
    }
    if (total_size < MAX_OBJECT_SIZE) {
      writer(url, content_buf);
    }
    
    Close(endserver_fd); // 식별자를 다 쓴 뒤, 닫아주기
    return;
}

// uri를 파싱해서 hostname, path, port번호에 값 맵핑
void parse_uri(char *uri, char *host, int *port, char *path) {
    *port = end_server_port; 
    char *pos = strstr(uri, "//");       // uri에 '//'가 있다면 자르기 위해 해당 위치를 pos에 저장함 ('http://'의 '//')
    pos = pos != NULL? pos + 2 : uri;    // pos안에 '//'가 있다면 포인터를 '//' 다음으로, 없다면 pos = uri

    char *pos2 = strstr(pos, ":");       // port번호를 분리하기 위해 ':'의 위치를 저장
    if (pos2 != NULL) { // port 번호 지정되어 있는 경우 (ex: localhost:8000/home.html)
      *pos2 = '\0';                      // ':'을 '\0'으로 변환 ('\0' : 문자열의 끝을 의미)
      sscanf(pos, "%s", host);           // host는 cut의 앞부분 -> ex : localhost
      sscanf(pos2 + 1, "%d%s", port, path);  // port : 8000, path = /home.html
    } else {           // port 번호가 지정되지 않은 경우 디폴트 포트 8000 사용
      pos2 = strstr(pos, "/"); 
      if (pos2 != NULL) { // path가 있는 경우
        *pos2 = '\0';
        sscanf(pos, "%s", host);
        *pos2 = '/';
        sscanf(pos2, "%s", path);
      } else {                              // host만 있는 경우
        sscanf(pos, "%s", host);
      }
    }
    if (strlen(host) == 0) strcpy(host, end_server_host);   // host명이 없는 경우 지정

    return;
}

/*
  proxy에서 서버로 http 헤더를 보낼 때 꼭 들어가야하는 헤더들 
  - Host header
  - User-Agent header
  - Connection header
  - Proxy_Connection header
*/
void build_http_header(char *http_header, char *hostname, char *path, rio_t *client_rio) 
{
  char buf[MAXLINE], request_hdr[MAXLINE], other_hdr[MAXLINE], host_hdr[MAXLINE];
  
  // request line
  sprintf(request_hdr, requestline_hdr_format, path); // request_hdr에 "GET %s HTTP/1.0\r\n 이 폼으로 경로를 넣어서 저장" 

  // get other request header for client rio and change it
  while (Rio_readlineb(client_rio, buf, MAXLINE) > 0) {  // 읽기에 성공한 경우 client_rio의 내용을 buf로 옮겨씀
    if (strcmp(buf, endof_hdr) == 0)                     // 종료 조건 : EOF
      break;
    
    if (!strncasecmp(buf, host_key, strlen(host_key))) { // buf의 내용이 host_key = "Host"이면 true
      strcpy(host_hdr, buf);                             // host_hdr에 복사
      continue;
    }
    // 필수로 들어가야 하는 헤더들
    if (strncasecmp(buf, connection_key, strlen(connection_key))                // buf의 내용이 ""
        &&strncasecmp(buf, proxy_connection_key, strlen(proxy_connection_key))
        &&strncasecmp(buf, user_agent_key, strlen(user_agent_key))) {
        strcat(other_hdr, buf); // buf를 남은 헤더에 연결
      }
  }
  if (strlen(host_hdr) == 0) { // 호스트 헤더가 비어있다면
    sprintf(host_hdr, host_hdr_format, hostname); // 호스트 이름을 "Host: %s\r\n" 폼에 넣고 Host_hdr에 출력
  }
  // http_header에 요청 헤더, 호스트 헤더, 연결 헤더, 프록시 헤더.... 들을 출력
  sprintf(http_header, "%s%s%s%s%s%s%s",
          request_hdr, 
          host_hdr,
          conn_hdr,
          prox_hdr,
          user_agent_hdr,
          other_hdr,
          endof_hdr);
  return;
}

/** 에러 메세지를 클라이언트에 보냄 */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXLINE], body[MAXBUF];

    sprintf(body, "<html><title>Proxy Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Proxy server</em>\r\n", body);

    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}

inline int connect_endServer(char *hostname, int port) {
  char portStr[100];
  sprintf(portStr, "%d", port);
  return Open_clientfd(hostname, portStr); // 클라이언트 입장에서 fd 열기. 서버와 연결 설정
}


void init_cache(void) {
  Sem_init(&mutex, 0, 1); // mutex를 1로 초기화 : mutex = 1
  Sem_init(&w, 0, 1);
  readcnt = 0;
  cache = (Cache_info *)Malloc(sizeof(Cache_info) * 10);
  
  for (int i=0; i < 10; i++) {
    cache[i].url = (char *)Malloc(sizeof(char) * 256);
    cache[i].flag = (int *)Malloc(sizeof(int));
    cache[i].cnt = (int *)Malloc(sizeof(int));
    cache[i].content = (char *)Malloc(sizeof(char) * MAX_OBJECT_SIZE);
    *(cache[i].flag) = 0;
    *(cache[i].cnt) = 0;
  }
}

int reader(int connfd, char *url) {
    int return_flag = 0;
    P(&mutex);
    readcnt++;
    if(readcnt == 1) {
        P(&w);
    }
    V(&mutex);

    for(int i = 0; i < 10; i++) {
        if(*(cache[i].flag) == 1 && !strcmp(cache[i].url, url)) {
            Rio_writen(connfd, cache[i].content, MAX_OBJECT_SIZE);
            return_flag = 1;
            *(cache[i].cnt) = 0;
            break;
        }
    }    
    
    for(int i = 0; i < 10; i++) {
        (*(cache[i].cnt))++;
    }

    P(&mutex);
    readcnt--;
    if(readcnt == 0) {
        V(&w);
    }
    V(&mutex);
    return return_flag;
}

void writer(char *url, char *buf) {
    int cache_cnt = 0;
    P(&w);

    for(int i = 0; i < 10; i++) {
        if(*(cache[i].flag) == 1 && !strcmp(cache[i].url, url)) {
            cache_cnt = 1;
            *(cache[i].cnt) = 0;
            break;
        }
    }

    if(cache_cnt == 0) {
        int idx = 0;
        int max_cnt = 0;
        for(int i = 0; i < 10; i++) {
            if(*(cache[i].flag) == 0) {
                idx = i;
                break;
            }
            if(*(cache[i].cnt) > max_cnt) {
                idx = i;
                max_cnt = *(cache[i].cnt);
            }
        }
        *(cache[idx].flag) = 1;
        strcpy(cache[idx].url, url);
        strcpy(cache[idx].content, buf);
        *(cache[idx].cnt) = 0;
    }
    for(int i = 0; i < 10; i++) {
        (*(cache[i].cnt))++;
    }
    V(&w);
}