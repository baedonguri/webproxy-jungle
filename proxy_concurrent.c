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
void *thread(void *vargs);

int main(int argc, char **argv) {
    int listenfd, *connfdp;
    socklen_t clientlen;
    char clienthost[MAXLINE], clientport[MAXLINE];
    struct sockaddr_storage clientaddr;
    pthread_t tid; // 메인 쓰레드의 주소를 저장할 tid 변수

    /* Check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);  // 지정한 포트 번호로 듣기 식별자 생성
    
    while (1) {
      clientlen = sizeof(clientaddr);
      

      
      connfdp = Malloc(sizeof(int)); // 메모리 동적할당
      *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
      
      Getnameinfo((SA *)&clientaddr, clientlen, clienthost, MAXLINE, clientport, MAXLINE, 0);
      printf("Accepted connection from (%s, %s)\n", clienthost, clientport);
      Pthread_create(&tid, NULL, thread, (void *)connfdp); // 프로세스 내에서 쓰레드 만들기
    }
    return 0;
}

void *thread(void *vargs)
{
  int connfd = *((int *)vargs);
  Pthread_detach(pthread_self()); // 연결 가능한 쓰레드 tid 분리. pthread_self()를 인자로 넣으면 자신을 분리
  Free(vargs);
  doit(connfd);
  Close(connfd);
  return NULL;
}

/** 한 개의 HTTP 트랜잭션을 처리 */
void doit(int fd) {
    int endserver_fd;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char endserver_http_header[MAXLINE];
    char hostname[MAXLINE], path[MAXLINE];
    int port;

    /*
      Rio의 장점 : EOF을 만났을 때 short count가 나옴을 보장한다. (short count : 원하는만큼 읽겠다고 했는데 다 못 읽는 경우에 발생)
      rio : 클라이언트와 프록시 사이의 버퍼
      server_rio : 프록시와 서버 사이의 버퍼
    */
    rio_t rio, endserver_rio;
    Rio_readinitb(&rio, fd); // rio와 connfd 연결
    Rio_readlineb(&rio, buf, MAXLINE); // 클라이언트가 보낸 요청라인을 읽고 분석. buf에 복사함
    sscanf(buf, "%s %s %s", method, uri, version);
        
    if (strcasecmp(method, "GET")) {   // 결과가 0이 아닐 경우임. 즉, GET method만 처리
        clienterror(fd, method, "501", "Not implemented", "Proxy does not implement this method");
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

    Rio_readinitb(&endserver_rio, endserver_fd); // 프록시와 서버 연결
    // 프록시에서 만든 http 헤더를 endserver로 전달
    Rio_writen(endserver_fd, endserver_http_header, strlen(endserver_http_header));
    
    size_t n; // 버퍼에다 쓴 내용의 크기
    while ((n = Rio_readlineb(&endserver_rio, buf, MAXLINE)) != 0) {          // end server의 응답을 buf에 받기
        printf("Proxy received %ld bytes, then send\n", n);                   // proxy에 end server에서 받은 문자수를 출력하고
        Rio_writen(fd, buf, n);                                               // client에 end server response를 출력
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
void build_http_header(char *http_header, char *hostname, char *path, rio_t *client_rio) {
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

