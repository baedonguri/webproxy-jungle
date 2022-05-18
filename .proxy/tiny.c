/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */

/* 
  - 접속 주소 : 43.200.4.19:8000
  - 서버 안죽었을 시 : kill -9 [pid값]
  - 서버 프로세스 확인 : netstat -tnlp
*/

#include "csapp.h"

void doit(int fd); // 한개의 HTTP 트랜잭션 처리
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize, char *method);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs, char *method);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

/*  Rio_I/O
    - Rio_writen(rio_t *rp, void *usrbuf, size_t maxlen) : 서버->클라이언트, 클라이언트->서버로 데이터를 전송할 때 사용하는 함수
      -> 쓰기 진행 시 항상 버퍼 크기만큼 쓴다. 중간에 인터럽트가 생겨도 버퍼 크기만큼 쓰도록 보장.

    - Rio_readlineb(rio_t *rp, void *usrbuf, size_t n) : 한줄만 입력받기 위해 사용하는 함수
      -> 텍스트 줄을 파일(rp)에서 부터 읽고, 읽은 것을을 메모리 위치 usrbuf로 복사하고, 읽은 텍스트 라인을 NULL 문자로 바꾸고 종료시킴
      -> 최대 maxlen-1 개의 바이트를 읽는데, 이를 넘는 텍스트 라인들은 잘라서 NULL 문자로 종료시킴
      -> '\n' 개행 문자를 만날 경우 maxlen이 되기 전에 break됨
*/

/*
  문자열 관련 함수들

  1. sscanf 함수
  int sscanf(const char* str, const char* format, ...)
  -> ex) sscanf(buf, "%s %s %s", method, uri, version)
  -> buf 변수에서 데이터(문자열)를 가져와 해당 포맷팅에 맞추어 각 변수에 저장


  2. strcasecmp 함수
    - strcmp(str1,str2) : 문자열 비교 함수
    - strcasecmp() : 대소문자를 무시하는 문자열 비교 함수
    - strncasecmp() : 대소문자를 무시하고, 지정한 길이만큼 문자열을 비교하는 함수

  3. strstr 함수
  -> 문자열안에서 문자열을 검색하는 함수
*/

/*
  입력 : ./tiny 8000
  argc = 2, argv[0] = tiny, argv[1] = 8000
*/
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

  /*
    - listen socket open
    - Open_listenfd 함수를 호출해서 듣기 소켓을 오픈한다. 인자로 포트번호를 넘겨준다.
    - Open_listenfd는 요청받을 준비가 된 듣기 식별자를 리턴 = listenfd
  */
  listenfd = Open_listenfd(argv[1]); 
  // 무한루프 실행
  while (1) {
    clientlen = sizeof(clientaddr); // accpet 함수 인자에 넣기 위한 주소 길이를 계산
    
    /* Accept(듣기 식별자, 소켓주소구조체의 주소, 주소(소켓주소구조체)의 길이) 
      listenfd로 바인딩된 연결에 연결 요청이 도착할 때까지 기다린 다음, 주소 및 소켓 주소 크기로 입력한다.
      클라이언트와 통신하는 데 사용할 수 있는 연결자를 반환한다.
    */
    connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);  /* 반복적으로 연결요청 접수 */
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0); // 소켓주소 구조체에서 스트링 표시로 전환

    printf("Accepted connection from (%s, %s)\n", hostname, port);
    doit(connfd);   // 트랜잭션 수행
    Close(connfd);  // 트랜잭션 수행 후, 자신 쪽의 연결 끝을 닫는다.  
  }
}
/* 한개의 HTTP 트랜잭션을 처리하는 함수 */
void doit(int fd)
{
  int is_static;
  struct stat sbuf;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char filename[MAXLINE], cgiargs[MAXLINE];

  rio_t rio; // rio_readlineb를 위해 rio_t 타입(구조체)의 읽기 버퍼를 선언

  // RIO = Robust I/O 
  Rio_readinitb(&rio, fd); // rio_t 구조체를 초기화 줌

  Rio_readlineb(&rio, buf, MAXLINE); /* 한줄 단위로 입력을 받음. 이때 파라미터로 입력의 최대 바이트 크기를 넣어주어야함 
                                        newline(개행문자)을 만난 경우에 종료 */
  printf("Request headers:\n");
  printf("%s", buf);
  sscanf(buf, "%s %s %s", method, uri, version); // buf 변수에서 데이터(문자열)를 가져와 해당 포맷팅에 맞추어 각 변수에 저장
  // 입력된 method가 GET이나 Head가 아니라면 error
  // HEAD 메소드는 GET과 유사한 방식이지만, 웹 서버에서 헤더 정보 이외에는 어떠한 데이터도 보내지 않는다.
  if (!(strcasecmp(method, "GET") == 0 || strcasecmp(method, "HEAD") == 0)) {  // method 스트링이 GET이 아니면 0이 아닌 값이 나옴
    clienterror(fd, method, "501", "Not implemented", "Tiny does not implement this method");
    return;
  }
  
  // GET이라면 읽어들이고,다른 요청 헤더들을 무시
  read_requesthdrs(&rio);

  /* Parse URI from GET request */
  // GET 요청으로부터 온 URI를 분석, (파일 이름과 비어있을 수 있는 CGI 인자 스트링으로 분석, 요청이 정적 또는 동적 컨텐츠를 위한 것인지 나타내는 플래그를 설정)
  is_static = parse_uri(uri, filename, cgiargs); // cgiargs : 동적 컨텐츠의 실행파일에 들어갈 인자
  if(stat(filename, &sbuf) < 0) { // stat함수 : 파일의 정보를 얻는 함수, sbuf : 파일의 상태 및 정보를 저장할 buf 구조체, 0 : 정상, -1 : 오류(디스크에 존재하지 않을 경우)
    clienterror(fd, filename, "404", "Not found", "Tiny couldn't find this file");
    return;
  }
  if (is_static) { // 정적파일이라면
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) { // (일반 파일 여부) || (읽기 권한 여부 & 일반 파일 여부)
      // st_mode : 파일 종류와 file에 대한 access 권한 정보를 확인하는 POSIX macro
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't read the file");
      return ;
    }
    serve_static(fd, filename, sbuf.st_size, method); // st_size : 파일의 크기(byte)
  }
  else { // 동적파일이라면
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) { // (일반 파일인가?) || (실행 권한이 있나?) -> 정상이면 0이 리턴되기 때문
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't run the CGI program");
      return;
    }
    serve_dynamic(fd, filename, cgiargs, method); // 클라이언트에게 동적 컨텐츠를 제공
  }
}
// 일반파일(Regular File) : 2진수, text 등의 데이터를 담고 있음. OS에서 이 파일을 보면 정확한 포맷은 알지 못한 채, 그저 "일련의 바이트"라고 생각한다고 함.

/* 오류를 클라이언트에게 보고하는 함수 */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
    char buf[MAXLINE], body[MAXBUF];
    /* Build the HTTP response body */
    // sprintf() : 출력하는 결과값을 변수에 저장하게 해주는 기능
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

    /* Print the HTTP response */
    sprintf(buf, "HTTP1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf)); 
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}

/* Tiny는 요청 헤더 내의 어떠한 정보도 사용하지 않음.
  단순히 요청헤더를 읽고 무시하는 함수 */
void read_requesthdrs(rio_t *rp)
{
  char buf[MAXLINE];
  Rio_readlineb(rp, buf, MAXLINE);
  /* 헤더의 마지막 줄은 비어있기 때문에 \r\n만 buf에 남아있다면 while문을 탈출한다 */
  /* 버퍼 rp의 마지막 끝을 만날 때까지("Content-length: %d\r\n\r\n에서 마지막 \r\n) */
  while(strcmp(buf, "\r\n")) {
    // readlineb는 \n을 만날 때 멈춘다
    Rio_readlineb(rp, buf, MAXLINE); // 계속 출력해주면서 없앤다고 이해하면 될듯?
    printf("%s", buf);
    // 멈춘 지점까지 출력하고 다시 반복
  }
  return;
}

int parse_uri(char *uri, char *filename, char *cgiargs)
{
  char *ptr;
  
  if (!strstr(uri, "cgi-bin")) { // 정적 컨텐츠일 경우 // ststr : 문자열 안에서 문자열로 검색하기
    strcpy(cgiargs, ""); // cgi 인자 스트링 지우기
    strcpy(filename, "."); 
    strcat(filename, uri); // 상대 리눅스 파일 경로 이름으로 변경
    if (uri[strlen(uri)-1] == '/') // 만일 URI이 '/' 문자로 끝난다면 기본 파일 이름을 추가한다.
      strcat(filename, "home.html");
    return 1;
  }
  else { // 동적 컨텐츠일 경우
    ptr = index(uri, '?'); // 문자열 중에 '?'의 위치 찾기
    if (ptr) { // cgi 인자들 추출
      strcpy(cgiargs, ptr+1); 
      *ptr = '\0';
    }
    else
      strcpy(cgiargs, "");
    strcpy(filename, ".");
    strcat(filename, uri); // 상대 리눅스 파일 경로로 변환
    return 0;
  }
}

/* 
  정적 컨텐츠를 클라이언트로 보내는 함수
   Tiny는 HTML, 무형식 텍스트 파일, GIF, PNG, JPEG로 인코딩된 영상을 정적 컨텐츠 타입을 지원
*/
void serve_static(int fd, char *filename, int filesize, char *method)
{
  int srcfd;
  char *srcp, filetype[MAXLINE], buf[MAXBUF];

  /* Sent response headers to client */
  get_filetype(filename, filetype); // 파일 타입 결정하기 (파일 이름의 접미어 부분을 검사)
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
  sprintf(buf, "%sConnection: close\r\n", buf);
  sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
  sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
  /* 응답 라인과 헤더를 클라이언트에게 보냄 */
  Rio_writen(fd, buf, strlen(buf)); // connfd를 통해 clientfd에게 보냄
  printf("Response headers:\n");
  printf("%s", buf); // 서버 출력
  
  // 만약 메서드가 HEAD라면, 응답 본체를 만들지 않고 끝낸다.
  if (strcasecmp(method, "HEAD") == 0)
    return;

  /* Send response body to client */
  srcfd = Open(filename, O_RDONLY, 0); // 식별자 얻어오기 (읽기전용으로 연다)
  // srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
  srcp = (char*)Malloc(filesize); // 파일 크기만큼 메모리를 동적 할당 함
  Rio_readn(srcfd, srcp, filesize); // filename 내용을 동적할당한 메모리에 쓴다.

  Close(srcfd); // 매핑 후엔 식별자가 필요없으므로 파일 닫아주기 (닫지 않으면 메모리 누수 발생 가능성)
  Rio_writen(fd, srcp, filesize); // 파일을 클라이언트에 전달 (fd : 클라이언트의 연결 식별자)(로 복사)
  // Munmap(srcp, filesize); // 매핑된 가상메모리 주소 반환
  free(srcp);
}

void serve_dynamic(int fd, char *filename, char *cgiargs, char *method)
{
  char buf[MAXLINE], *emptylist[] = {NULL};

  /* Return first part of HTTP response */
  sprintf(buf, "HTTP/1.0 200 OK\r\n");
  Rio_writen(fd, buf, strlen(buf));
  sprintf(buf, "Server: Tiny Web Server\r\n");
  Rio_writen(fd, buf, strlen(buf));

  // fork를 실행하면 부모 프로세스와 자식 프로세스가 동시에 실행
  if (Fork() == 0) { // fork의 반환값이 0이라면 (자식 프로세스) if문 수행
    /* Real server would set all CGI vars here */
    setenv("QUERY_STRING", cgiargs, 1); // setenv 시스템콜을 수행해, "QUERY_STRING"의 값을 cgiargs로 바꿔줌. 우선순위가 1이므로 기존의 값과 상관없이 값이 변경
    
    // 요청 메서드를 환경 변수에 추가하기
    setenv("REQUEST_METHOD", method, 1);

    Dup2(fd, STDOUT_FILENO);  // CGI 프로세스의 표준 출력을 fd로 복사. STDOUT_FILENO의 값이 저장. 즉, CGI 프로세스에서 표준 출력을 하면 fd를 거쳐 클라이언트에 출력
    Execve(filename, emptylist, environ); // 파일이름이 filename인 파일을 실행함(CGI 프로그램 로드 후 실행)
  }
  Wait(NULL); // 부모 컨텍스트는 자식 컨텍스트가 종료되고 정리되는 것을 기다리기 위해 블록됨

}


void get_filetype(char *filename, char *filetype)
{
  if(strstr(filename, ".html"))
    strcpy(filetype, "text/html");
  else if (strstr(filename, ".gif"))
    strcpy(filetype, "image/gif");
  else if (strstr(filename, ".png"))
    strcpy(filetype, "image/png");
  else if (strstr(filename, ".jpg"))
    strcpy(filetype, "image/jpeg");
  else if (strstr(filename, ".mp4"))
    strcpy(filetype, "video/mp4");
  else
    strcpy(filetype, "text/plain");
}