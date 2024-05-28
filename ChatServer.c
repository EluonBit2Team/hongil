#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <pthread.h>

#define BUF_SIZE 1024
#define EPOLL_SIZE 50
#define THREAD_NUM 4

void setnonblockingmode(int fd);
void error_handling(char *buf);
void *worker_thread(void *arg);
void broadcast_message(int sender_fd, char *message, int length);

int serv_sock;
int epfd;
struct epoll_event *ep_events;
pthread_mutex_t client_list_mutex;
pthread_mutex_t epoll_mutex;
int *client_list;
int client_count = 0;
int client_capacity = 10;

void add_client_socket(int clnt_sock);
void remove_client_socket(int clnt_sock);

int main(int argc, char *argv[]) {
    struct sockaddr_in serv_adr;
    int i;
    struct epoll_event event;

    if(argc != 2) {
        printf("Usage : %s <port>\n", argv[0]);
        exit(1);
    }

    serv_sock = socket(PF_INET, SOCK_STREAM, 0);
    if (serv_sock == -1) {
        error_handling("socket() error");
    }

    memset(&serv_adr, 0, sizeof(serv_adr));
    serv_adr.sin_family = AF_INET;
    serv_adr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_adr.sin_port = htons(atoi(argv[1]));

    if(bind(serv_sock, (struct sockaddr*)&serv_adr, sizeof(serv_adr)) == -1) {
        error_handling("bind() error");
    }
    if(listen(serv_sock, 5) == -1) {
        error_handling("listen() error");
    }
    printf("서버가 실행되었습니다.\n");

    //epoll 생성
    epfd = epoll_create(EPOLL_SIZE);
    if (epfd == -1) {
        error_handling("epoll_create() error");
    }

    ep_events = (struct epoll_event*)malloc(sizeof(struct epoll_event) * EPOLL_SIZE);
    if (ep_events == NULL) {
        error_handling("malloc() error");
    }

    setnonblockingmode(serv_sock);

    //epoll 이벤트 등록
    event.events = EPOLLIN;
    event.data.fd = serv_sock;
    
    pthread_mutex_init(&client_list_mutex, NULL);
    pthread_mutex_init(&epoll_mutex, NULL);

    pthread_mutex_lock(&epoll_mutex);
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, serv_sock, &event) == -1){
        error_handling("epoll_ctl() error");
    }
    pthread_mutex_unlock(&epoll_mutex);

    client_list = (int*)malloc(sizeof(int) * client_capacity);
    if (client_list == NULL) {
        error_handling("malloc() error");
    }

    pthread_mutex_init(&client_list_mutex, NULL);
    // 스레드풀 추가
    pthread_t threads[THREAD_NUM];
    for(i = 0; i < THREAD_NUM; i++) {
        if(pthread_create(&threads[i], NULL, worker_thread, NULL) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }

    for(i = 0; i < THREAD_NUM; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_mutex_destroy(&client_list_mutex);
    pthread_mutex_destroy(&epoll_mutex);
    close(serv_sock);
    close(epfd);
    free(ep_events);
    free(client_list);
    return 0;
}

void *worker_thread(void *arg)
{
    int event_cnt, i, str_len;
    char buf[BUF_SIZE];

    while(1) {
        //epoll 변화가 일어난 이벤트 개수
        event_cnt = epoll_wait(epfd, ep_events, EPOLL_SIZE, -1);
        if(event_cnt == -1) {
            puts("epoll_wait() error");
            return NULL;
        }

        for(i = 0; i < event_cnt; i++) {
            if(ep_events[i].data.fd == serv_sock) {
                struct sockaddr_in clnt_adr;
                socklen_t adr_sz = sizeof(clnt_adr);
                int clnt_sock = accept(serv_sock, (struct sockaddr*)&clnt_adr, &adr_sz);
                if (clnt_sock == -1) {
                    perror("accept() error");
                    continue;
                }

                setnonblockingmode(clnt_sock);
                struct epoll_event event;
                event.events = EPOLLIN | EPOLLET;
                event.data.fd = clnt_sock;

                pthread_mutex_lock(&epoll_mutex);
                if (epoll_ctl(epfd, EPOLL_CTL_ADD, clnt_sock, &event) == -1) {
                    perror("epoll_ctl() error");
                    close(clnt_sock);
                    continue;
                }
                pthread_mutex_unlock(&epoll_mutex);
                //연결 성공시 클라이언트 소켓 목록에 추가
                add_client_socket(clnt_sock);
                printf("connected client: %d \n", clnt_sock);
            }
            else {
                while(1) {
                    //받아온 데이터 없음 = 종료
                    str_len = read(ep_events[i].data.fd, buf, BUF_SIZE);
                    if(str_len == 0) {
                        epoll_ctl(epfd, EPOLL_CTL_DEL, ep_events[i].data.fd, NULL);
                        close(ep_events[i].data.fd);
                        //종료시 클라이언트 소켓 목록에서 제거
                        remove_client_socket(ep_events[i].data.fd);
                        printf("closed client: %d \n", ep_events[i].data.fd);
                        break;
                    }
                    //데이터 받아오기 실패
                    else if(str_len < 0) {
                        if(errno == EAGAIN)
                            break;
                    }
                    //받았으면 나를 제외한 다른 클라이언트에게 전송
                    else {
                        broadcast_message(ep_events[i].data.fd, buf, str_len);
                    }
                }
            }
        }
    }
    return NULL;
}

void setnonblockingmode(int fd) {
    int flag = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

void error_handling(char *buf) {
    fputs(buf, stderr);
    fputc('\n', stderr);
    exit(1);
}

void broadcast_message(int sender_fd, char *message, int length) {
    pthread_mutex_lock(&client_list_mutex);
    for(int i = 0; i < client_count; i++) {
        int sock = client_list[i];
        if(sock != sender_fd) {
            write(sock, message, length);
        }
    }
    pthread_mutex_unlock(&client_list_mutex);
}

void add_client_socket(int clnt_sock) {
    pthread_mutex_lock(&client_list_mutex);
    if (client_count == client_capacity) {
        client_capacity *= 2;
        client_list = (int*)realloc(client_list, sizeof(int) * client_capacity);
        if (client_list == NULL) {
            error_handling("realloc() error");
        }
    }
    client_list[client_count++] = clnt_sock;
    pthread_mutex_unlock(&client_list_mutex);
}

void remove_client_socket(int clnt_sock) {
    pthread_mutex_lock(&client_list_mutex);
    for (int i = 0; i < client_count; i++) {
        if (client_list[i] == clnt_sock) {
            client_list[i] = client_list[--client_count];
            break;
        }
    }
    pthread_mutex_unlock(&client_list_mutex);
}
