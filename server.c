// server_epoll_thread_pool.c
// 使用 epoll 和线程池实现的多线程服务端
// 编译: gcc -O2 -Wall -o server_epoll_thread_pool server_epoll_thread_pool.c
// -pthread

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BACKLOG 16
#define BUFSIZE 4096
#define MAX_EVENTS 64
#define THREAD_POOL_SIZE 4

// 服务器配置结构体
struct server_config {
  int listen_fd;
  int epoll_fd;
  int port;
};

// 客户端请求处理结构体
struct client {
  int fd;
  FILE *fp;
};

// 任务队列
struct task {
  void (*function)(struct client *cl);
  struct client *cl;
  struct task *next;
};

// 线程池数据结构
struct thread_pool {
  pthread_t *threads;
  struct task *task_queue_head;
  struct task *task_queue_tail;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  int shutdown;
};

void *thread_worker(void *arg);

// 创建服务器监听套接字
int create_listen_fd(int port) {
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    perror("socket");
    return -1;
  }

  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(port);

  if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
      0) {
    perror("bind");
    close(listen_fd);
    return -1;
  }

  if (listen(listen_fd, BACKLOG) < 0) {
    perror("listen");
    close(listen_fd);
    return -1;
  }

  return listen_fd;
}

// 创建 epoll 实例并添加监听套接字
int create_epoll_fd(int listen_fd) {
  int epoll_fd = epoll_create1(0);
  if (epoll_fd == -1) {
    perror("epoll_create1");
    close(listen_fd);
    return -1;
  }

  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = listen_fd;

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
    perror("epoll_ctl");
    close(epoll_fd);
    close(listen_fd);
    return -1;
  }

  return epoll_fd;
}

// 启动线程池
struct thread_pool *create_thread_pool(int num_threads) {
  struct thread_pool *pool = malloc(sizeof(struct thread_pool));
  if (!pool) {
    perror("malloc");
    return NULL;
  }

  pool->threads = malloc(sizeof(pthread_t) * num_threads);
  if (!pool->threads) {
    perror("malloc");
    free(pool);
    return NULL;
  }

  pool->task_queue_head = pool->task_queue_tail = NULL;
  pool->shutdown = 0;
  pthread_mutex_init(&pool->lock, NULL);
  pthread_cond_init(&pool->cond, NULL);

  // 启动线程
  for (int i = 0; i < num_threads; i++) {
    if (pthread_create(&pool->threads[i], NULL, thread_worker, pool) != 0) {
      perror("pthread_create");
      free(pool->threads);
      free(pool);
      return NULL;
    }
    printf("thread %d init\n", i);
  }

  return pool;
}

// 销毁线程池
void destroy_thread_pool(struct thread_pool *pool) {
  if (!pool)
    return;

  pool->shutdown = 1;
  pthread_cond_broadcast(&pool->cond);

  for (int i = 0; i < THREAD_POOL_SIZE; i++) {
    pthread_join(pool->threads[i], NULL);
  }

  free(pool->threads);
  free(pool);
}

// 线程池工作线程
void *thread_worker(void *arg) {
  struct thread_pool *pool = (struct thread_pool *)arg;

  while (1) {
    pthread_mutex_lock(&pool->lock);

    // 等待任务
    while (!pool->task_queue_head && !pool->shutdown) {
      pthread_cond_wait(&pool->cond, &pool->lock);
    }

    if (pool->shutdown) {
      pthread_mutex_unlock(&pool->lock);
      break;
    }

    // 获取任务并从队列中移除
    struct task *t = pool->task_queue_head;
    pool->task_queue_head = pool->task_queue_head->next;
    if (!pool->task_queue_head) {
      pool->task_queue_tail = NULL;
    }

    pthread_mutex_unlock(&pool->lock);

    // 执行任务
    t->function(t->cl);
    free(t);
  }

  return NULL;
}

// 提交任务到线程池
void submit_task(struct thread_pool *pool, void (*function)(struct client *cl),
                 struct client *cl) {
  struct task *t = malloc(sizeof(struct task));
  t->function = function;
  t->cl = cl;
  t->next = NULL;

  pthread_mutex_lock(&pool->lock);

  if (pool->task_queue_tail) {
    pool->task_queue_tail->next = t;
  } else {
    pool->task_queue_head = t;
  }
  pool->task_queue_tail = t;

  pthread_cond_signal(&pool->cond);
  pthread_mutex_unlock(&pool->lock);
}

// 处理INFO请求
void handle_info_request(struct client *cl, char *filename) {

  // 解析请求，期待格式：INFO <filename>
  // 获取文件状态
  struct stat st;
  if (stat(filename, &st) != 0) {
    // 文件不存在
    if (errno == ENOENT) {
      fprintf(cl->fp, "ERR NOFILE\n");
    } else {
      fprintf(cl->fp, "ERR\n");
    }
    fflush(cl->fp);
  } else {
    // 文件存在，返回文件大小
    fprintf(cl->fp, "OK %lld\n", (long long)st.st_size);
    fflush(cl->fp);
  }
  close(cl->fd);
  free(cl);
}

// 发送文件数据
void send_file(struct client *cl) {
  char line[512];
  char filename[256];
  long long offset = 0;
  long long length = 0;

  // 从客户端读取请求 (例如: GET filename offset length)
  if (fgets(line, sizeof(line), cl->fp) == NULL) {
    fprintf(cl->fp, "ERR BADREQUEST\n");
    fflush(cl->fp);
    return;
  }

  if (sscanf(line, "INFO %255s", filename) == 1) {
    handle_info_request(cl, filename);
    return;
  }

  // 解析客户端请求
  if (sscanf(line, "GET %255s %lld %lld", filename, &offset, &length) != 3) {
    fprintf(cl->fp, "ERR BADREQUEST\n");
    fflush(cl->fp);
    return;
  }

  // 打开文件
  int filefd = open(filename, O_RDONLY);
  if (filefd < 0) {
    fprintf(cl->fp, "ERR NOFILE\n");
    fflush(cl->fp);
    return;
  }

  struct stat st;
  if (fstat(filefd, &st) < 0) {
    close(filefd);
    fprintf(cl->fp, "ERR\n");
    fflush(cl->fp);
    return;
  }

  // 验证偏移量和长度
  if (offset < 0 || offset > st.st_size) {
    close(filefd);
    fprintf(cl->fp, "ERR RANGE\n");
    fflush(cl->fp);
    return;
  }

  // 如果需要的长度超过文件剩余部分，只传输剩余部分
  if (offset + length > st.st_size) {
    length = st.st_size - offset;
  }

  // 通知客户端准备开始传输
  fprintf(cl->fp, "OK\n");
  fflush(cl->fp);

  // 开始传输文件
  off_t off = offset;
  ssize_t tosend = (ssize_t)length;
  while (tosend > 0) {
    ssize_t sent = sendfile(cl->fd, filefd, &off,
                            (size_t)(tosend > BUFSIZE ? BUFSIZE : tosend));
    if (sent <= 0) {
      if (errno == EINTR) {
        continue;
      }
      printf("client exir: %s\n", strerror(errno));
      break;
    }
    tosend -= sent;
  }

  // 清理，这里不复用 TCP 连接
  close(filefd);
  close(cl->fd);
  free(cl);
}

// 接受新的客户端连接并返回其文件描述符
int accept_client(int listen_fd, struct sockaddr_in *client_addr) {
  socklen_t client_len = sizeof(*client_addr);
  int client_fd =
      accept(listen_fd, (struct sockaddr *)client_addr, &client_len);
  if (client_fd == -1) {
    perror("accept");
    return -1;
  }

  int flags = fcntl(client_fd, F_GETFL, 0);
  fcntl(client_fd, F_SETFL, flags);

  return client_fd;
}

// 主服务器函数
void run_server(int listen_fd, int epoll_fd, struct thread_pool *pool) {
  struct epoll_event events[MAX_EVENTS];
  signal(SIGPIPE, SIG_IGN);

  while (1) {
    int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
    if (nfds == -1) {
      perror("epoll_wait");
      break;
    }

    for (int i = 0; i < nfds; i++) {
      if (events[i].data.fd == listen_fd) {
        // 新连接，接受并注册
        struct sockaddr_in client_addr;
        int client_fd = accept_client(listen_fd, &client_addr);
        if (client_fd == -1)
          continue;

        printf("New connection from %s:%d\n", inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));

        // 将客户端套接字添加到 epoll 监听
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = client_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
          perror("epoll_ctl");
          close(client_fd);
        }
      } else {
        // 处理已有客户端请求
        struct client *cl = (struct client *)malloc(sizeof(struct client));
        cl->fd = events[i].data.fd;
        cl->fp = fdopen(cl->fd, "r+");

        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, cl->fd, NULL) < 0) {
          perror("epoll_ctl");
        }

        if (cl->fp) {
          submit_task(pool, send_file, cl);
        }
      }
    }
  }
}

// 释放服务器资源
void cleanup(int listen_fd, int epoll_fd, struct thread_pool *pool) {
  destroy_thread_pool(pool);
  close(epoll_fd);
  close(listen_fd);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <port> [port]\n", argv[0]);
    return 1;
  }

  int port = atoi(argv[1]);
  int thread_pool_size = THREAD_POOL_SIZE;
  if (argc == 3) {
    thread_pool_size = atoi(argv[2]);
  }

  // 创建监听套接字和 epoll 实例
  int listen_fd = create_listen_fd(port);
  if (listen_fd == -1)
    return 1;

  int epoll_fd = create_epoll_fd(listen_fd);
  if (epoll_fd == -1) {
    close(listen_fd);
    return 1;
  }

  // 创建线程池
  struct thread_pool *pool = create_thread_pool(thread_pool_size);
  if (!pool) {
    close(listen_fd);
    close(epoll_fd);
    return 1;
  }

  // 启动服务器并运行
  printf("Server running on port %d...\n", port);
  run_server(listen_fd, epoll_fd, pool);

  // 清理资源
  cleanup(listen_fd, epoll_fd, pool);

  return 0;
}
