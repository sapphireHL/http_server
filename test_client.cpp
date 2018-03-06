#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>

// 向服务器发送的请求
static const char* request = "GET http://localhost/index.html HTTP/1.1\r\nConnection: keep-alive\r\n\r\n";

int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLOUT | EPOLLET | EPOLLERR;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//向服务器写入len字节数据
bool write_nbytes(int sockfd, const char* buf, int len)
{
    int bytes_write = 0;
    printf("write out %d bytes  to  socket %d\n", len, sockfd);
    while(1)
    {
        bytes_write = send(sockfd, buf, len, 0);
        if(bytes_write == -1)
            return false;
        else if(bytes_write == 0)
            return false;
        len -= bytes_write;
        buf = buf + bytes_write;
        if(len <= 0){
            return true;
        }
    }
}

//从服务器读取数据
bool read_once(int sockfd, char* buf,  int len)
{
    int bytes_read = 0;
    memset(buf, '\0', len);
    bytes_read =  recv(sockfd, buf, len, 0);
    if(bytes_read == -1 || bytes_read == 0)
        return false;
    printf("read in %d bytes from socket %d with content %s\n", bytes_read, sockfd, buf);
    return true;
}

//向服务器 发送num个tcp连接，通过改变num来调整测试压力
void start_conn(int epollfd, int num, const char*ip, int port)
{
    //int ret = 0;
    struct sockaddr_in adr;
    bzero(&adr, sizeof(adr));
    adr.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &adr.sin_addr);
    adr.sin_port = htons(port);

    for(int i = 0; i < num; i++){
        sleep(1);
        int sockfd = socket(PF_INET, SOCK_STREAM, 0);
        printf("create 1 sock.\n");
        if(sockfd < 0)
            continue;
        if(connect(sockfd, (struct sockaddr*)&adr, sizeof(adr)) == 0){
            printf("build connection.\n");
            addfd(epollfd, sockfd);
        }
    }
}

//关闭连接
void close_conn(int epollfd, int sockfd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, sockfd, NULL);
    printf("close conn:%d\n", sockfd);
    close(sockfd);
}


int main(int argc, char*argv[])
{
    assert(argc == 4);
    int epollfd = epoll_create(5);
    start_conn(epollfd, atoi(argv[3]), argv[1], atoi(argv[2]));
    epoll_event events[10000];
    char buf[2048];

    while(1){
        int fds = epoll_wait(epollfd, events, 10000, 2000);
        for(int i = 0; i < fds; i++){
            int sockfd = events[i].data.fd;
            if(events[i].events & EPOLLIN){
                if(!read_once(sockfd, buf, 2048))
                    close_conn(epollfd, sockfd);
                epoll_event event;
                event.events = EPOLLOUT |  EPOLLET | EPOLLERR;
                event.data.fd = sockfd;
                epoll_ctl(epollfd, EPOLL_CTL_MOD, sockfd, &event);
            }
            else if(events[i].events & EPOLLOUT){
                if(!write_nbytes(sockfd, request, strlen(request))){
                    close_conn(epollfd, sockfd);
                }
                epoll_event event;
                event.events = EPOLLIN |  EPOLLET | EPOLLERR;
                event.data.fd = sockfd;
                epoll_ctl(epollfd, EPOLL_CTL_MOD, sockfd, &event);
            }
            else if(events[i].events & EPOLLERR){
                printf("err!\n");
                close_conn(epollfd, sockfd);
            }
        }
    }
    return 0;
}
