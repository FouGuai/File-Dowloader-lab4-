// Copyright (c) 2025 foushen. All rights reserved.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_PARTS 64
#define BUFSIZE 8192

typedef struct {
  long long start;
  long long len;
  int done; // 0/1
} part_t;

typedef struct {
  char server_ip[64];
  int server_port;
  char filename[512];
  char outpath[512];
  int parts;
  part_t parts_arr[MAX_PARTS];
  pthread_mutex_t meta_lock;
  char meta_path[1024];
} job_t;

typedef struct {
  job_t *job;
  int part_idx;
} thread_arg_t;

static int connect_server(const char *ip, int port) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0)
    return -1;
  struct sockaddr_in srv;
  memset(&srv, 0, sizeof(srv));
  srv.sin_family = AF_INET;
  srv.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &srv.sin_addr) <= 0) {
    close(s);
    return -1;
  }
  if (connect(s, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
    close(s);
    return -1;
  }
  return s;
}

// read single-line response from fd (FILE*)
static int read_line_fp(FILE *fp, char *buf, size_t sz) {
  if (!fgets(buf, sz, fp))
    return -1;
  char *p = strchr(buf, '\n');
  if (p)
    *p = '\0';
  return 0;
}

// load/save meta: simple text per line "start len\n" for completed parts
static void save_meta(job_t *job) {
  pthread_mutex_lock(&job->meta_lock);
  FILE *f = fopen(job->meta_path, "w");
  if (!f) {
    pthread_mutex_unlock(&job->meta_lock);
    return;
  }
  for (int i = 0; i < job->parts; i++) {
    if (job->parts_arr[i].done) {
      fprintf(f, "%lld %lld\n", (long long)job->parts_arr[i].start,
              (long long)job->parts_arr[i].len);
    }
  }
  fflush(f);
  fclose(f);
  pthread_mutex_unlock(&job->meta_lock);
}

static void load_meta(job_t *job) {
  pthread_mutex_lock(&job->meta_lock);
  FILE *f = fopen(job->meta_path, "r");
  if (!f) {
    pthread_mutex_unlock(&job->meta_lock);
    return;
  }
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    long long s, l;
    if (sscanf(line, "%lld %lld", &s, &l) == 2) {
      for (int i = 0; i < job->parts; i++) {
        if (job->parts_arr[i].start == s && job->parts_arr[i].len == l) {
          job->parts_arr[i].done = 1;
          break;
        }
      }
    }
  }
  fclose(f);
  pthread_mutex_unlock(&job->meta_lock);
}

static void *download_part(void *arg) {
  thread_arg_t *t = (thread_arg_t *)arg;
  job_t *job = t->job;
  int idx = t->part_idx;
  free(t);

  if (job->parts_arr[idx].done)
    return NULL;

  long long start = job->parts_arr[idx].start;
  long long len = job->parts_arr[idx].len;

  int s = connect_server(job->server_ip, job->server_port);
  if (s < 0) {
    fprintf(stderr, "connect failed for part %d\n", idx);
    return NULL;
  }
  FILE *fp = fdopen(s, "r+");
  if (!fp) {
    close(s);
    return NULL;
  }

  // send GET
  fprintf(fp, "GET %s %lld %lld\n", job->filename, start, len);
  fflush(fp);
  char line[256];
  if (read_line_fp(fp, line, sizeof(line)) != 0) {
    fclose(fp);
    return NULL;
  }
  if (strncmp(line, "OK", 2) != 0) {
    fprintf(stderr, "server error on part %d: %s\n", idx, line);
    fclose(fp);
    return NULL;
  }

  // open output file
  int outfd = open(job->outpath, O_CREAT | O_WRONLY, 0666);
  if (outfd < 0) {
    fprintf(stderr, "open out failed\n");
    fclose(fp);
    return NULL;
  }

  // seek writing using pwrite
  long long remaining = len;
  char buf[BUFSIZE];
  while (remaining > 0) {
    ssize_t toread = remaining > BUFSIZE ? BUFSIZE : remaining;
    ssize_t r = fread(buf, 1, toread, fp);
    if (r <= 0) {
      if (feof(fp))
        break;
      if (ferror(fp)) {
        perror("recv");
        break;
      }
    }

    ssize_t w = pwrite(outfd, buf, r, start + (len - remaining));
    if (w < 0) {
      perror("pwrite");
      break;
    }
    remaining -= r;
  }
  close(outfd);
  fclose(fp);

  if (remaining == 0) {
    // mark done and save meta
    pthread_mutex_lock(&job->meta_lock);
    job->parts_arr[idx].done = 1;
    pthread_mutex_unlock(&job->meta_lock);
    save_meta(job);
    printf("part %d done (start=%lld len=%lld)\n", idx, start, len);
  } else {
    fprintf(stderr, "part %d incomplete, remaining=%lld\n", idx, remaining);
  }

  return NULL;
}

int main(int argc, char **argv) {
  if (argc < 6) {
    fprintf(stderr,
            "Usage: %s <server_ip> <port> <remote_filename> <local_outpath> "
            "<num_threads>\n",
            argv[0]);
    return 1;
  }
  job_t job;
  memset(&job, 0, sizeof(job));
  strncpy(job.server_ip, argv[1], sizeof(job.server_ip) - 1);
  job.server_port = atoi(argv[2]);
  strncpy(job.filename, argv[3], sizeof(job.filename) - 1);
  strncpy(job.outpath, argv[4], sizeof(job.outpath) - 1);
  job.parts = atoi(argv[5]);
  if (job.parts < 1) {
    job.parts = 1;
  }

  if (job.parts > MAX_PARTS) {
    job.parts = MAX_PARTS;
  }

  pthread_mutex_init(&job.meta_lock, NULL);
  snprintf(job.meta_path, sizeof(job.meta_path), "%s.meta", job.outpath);

  // connect to server to get file size
  int s = connect_server(job.server_ip, job.server_port);
  if (s < 0) {
    fprintf(stderr, "connect failed\n");
    return 1;
  }
  FILE *fp = fdopen(s, "r+");
  if (!fp) {
    close(s);
    return 1;
  }
  fprintf(fp, "INFO %s\n", job.filename);
  fflush(fp);
  char line[256];
  if (read_line_fp(fp, line, sizeof(line)) != 0) {
    fprintf(stderr, "no response\n");
    fclose(fp);
    return 1;
  }
  long long filesize = -1;
  if (strncmp(line, "OK ", 3) == 0) {
    sscanf(line + 3, "%lld", &filesize);
  } else {
    fprintf(stderr, "server error: %s\n", line);
    fclose(fp);
    return 1;
  }
  fclose(fp);

  // create/truncate local file to filesize (sparse)
  int outfd = open(job.outpath, O_CREAT | O_WRONLY, 0666);
  if (outfd < 0) {
    perror("open out");
    return 1;
  }
  if (ftruncate(outfd, filesize) != 0) {
    perror("ftruncate");
    close(outfd);
    return 1;
  }
  close(outfd);

  // split into parts
  long long base = filesize / job.parts;
  long long rem = filesize % job.parts;
  long long cur = 0;
  for (int i = 0; i < job.parts; i++) {
    long long partlen = base + (i < rem ? 1 : 0);
    job.parts_arr[i].start = cur;
    job.parts_arr[i].len = partlen;
    job.parts_arr[i].done = 0;
    cur += partlen;
  }

  // load meta
  load_meta(&job);

  // create threads for each part that is not done
  pthread_t tids[MAX_PARTS];
  int started = 0;
  for (int i = 0; i < job.parts; i++) {
    if (job.parts_arr[i].done) {
      printf("part %d already done, skip\n", i);
      continue;
    }
    thread_arg_t *targ = malloc(sizeof(*targ));
    targ->job = &job;
    targ->part_idx = i;
    if (pthread_create(&tids[i], NULL, download_part, targ) != 0) {
      perror("pthread_create");
      free(targ);
    } else {
      started++;
    }
  }

  // join threads
  for (int i = 0; i < job.parts; i++) {
    if (!job.parts_arr[i].done) {
      pthread_join(tids[i], NULL);
    }
  }

  // check all done
  int all = 1;
  for (int i = 0; i < job.parts; i++) {
    if (!job.parts_arr[i].done) {
      all = 0;
    }
  }
  if (all) {
    // remove meta
    unlink(job.meta_path);
    printf("Download complete: %s (%lld bytes)\n", job.outpath, filesize);
  } else {
    printf("Download incomplete, resume later. See %s\n", job.meta_path);
  }

  pthread_mutex_destroy(&job.meta_lock);
  return 0;
}
