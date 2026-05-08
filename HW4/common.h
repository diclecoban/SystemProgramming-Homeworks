#ifndef HW4_COMMON_H
#define HW4_COMMON_H

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <semaphore.h>
#include <sys/syscall.h>
#endif

/* Programin sabit ust limitleri; diziler bu limitlere gore stack/shared memory'de ayrilir. */
#define MAX_KEYWORDS 8
#define MAX_WORKERS 64
#define MAX_LEVELS 4
#define MAX_SOURCES 256
#define MAX_FILES 128
#define MAX_PATH_LEN 512
#define MAX_MESSAGE_LEN 8192
#define MAX_LEVEL_NAME 8
#define MAGIC_CHECKPOINT 0xC5E3440BU

/* Log seviyeleri hem string hem de index olarak kullanilir; index dizilere erisim icindir. */
typedef enum {
    LEVEL_ERROR = 0,
    LEVEL_WARN = 1,
    LEVEL_INFO = 2,
    LEVEL_DEBUG = 3,
    LEVEL_INVALID = -1
} log_level_t;

/* Reader tarafindan parse edilen ve kuyruklar arasinda tasinan tek log kaydi. */
typedef struct {
    char timestamp[20];              /* [YYYY-MM-DD HH:MM:SS] formatindaki zaman bilgisi. */
    char level[MAX_LEVEL_NAME];      /* ERROR/WARN/INFO/DEBUG string hali. */
    char source[64];                 /* Logu ureten kaynak; priority filter bu alana bakar. */
    char message[MAX_MESSAGE_LEN];   /* Keyword aramasinin yapildigi log mesaji. */
    int level_index;                 /* level stringinin enum/index karsiligi. */
    int is_eof;                      /* Reader'in bu level icin dosya sonu marker'i gonderdigini belirtir. */
    int reader_id;                   /* Kaydin hangi reader process/log dosyasindan geldigini tutar. */
} log_entry_t;

/* Analyzer process'lerinin urettigi ve aggregator'in rapora yazdigi level bazli sonuc. */
typedef struct {
    char level[8];                            /* Sonucun ait oldugu level adi. */
    long total_entries;                       /* Bu level icin islenen toplam log sayisi. */
    double total_weighted_score;              /* Tum keyword skorlarinin level agirlikli toplami. */
    double per_keyword_score[MAX_KEYWORDS];   /* Keyword bazinda agirlikli skorlar. */
    double per_thread_score[MAX_WORKERS];     /* Worker thread bazinda skor katkisi. */
    char top_source[3][64];                   /* En cok skor ureten ilk uc source adi. */
    long top_source_hits[3];                  /* top_source alanlarina karsilik gelen skor/hit degeri. */
    int ready;                                /* Analyzer bu sonucu tamamlayinca 1 olur. */
} level_result_t;

/* Binary checkpoint dosyasinin basina yazilan metadata. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t num_levels;
    uint32_t num_keywords;
    double total_weighted;
    double high_priority_weighted;
} checkpoint_header_t;

/* Region A: Reader process'lerinden Dispatcher'a giden ortak input kuyrugu. */
typedef struct {
    int head;                                /* Circular buffer'da okunacak ilk eleman. */
    int tail;                                /* Circular buffer'da yazilacak ilk bos yer. */
    int count;                               /* Kuyruktaki aktif eleman sayisi. */
    int capacity;                            /* entries[] kapasitesi. */
    int total_readers;                       /* Toplam reader/log dosyasi sayisi. */
    int eof_count_per_level[MAX_LEVELS];     /* Her level icin kac reader EOF marker gonderdi. */
    pthread_mutex_t input_mutex;             /* Region A kuyrugunu process'ler arasi korur. */
    pthread_cond_t not_full_a;               /* Kuyruk doluysa producer'lar bunu bekler. */
    pthread_cond_t not_empty_a;              /* Kuyruk bossa dispatcher bunu bekler. */
    log_entry_t entries[];                   /* Flexible array: mmap boyutuna gore log_entry_t tutar. */
} region_a_t;

/* Region B: Dispatcher'dan Analyzer'lara giden, her level icin ayri ortak kuyruk. */
typedef struct {
    int head;                    /* Analyzer'in okuyacagi siradaki entry indexi. */
    int tail;                    /* Dispatcher'in yazacagi siradaki bos index. */
    int count;                   /* Kuyrukta bekleyen entry sayisi. */
    int capacity;                /* entries[] kapasitesi. */
    int eof_posted;              /* Bu level icin tum reader EOF'lari geldiyse dispatcher 1 yapar. */
    pthread_mutex_t level_mutex; /* Bu level kuyrugunu process'ler arasi korur. */
    pthread_cond_t not_full_b;   /* Kuyruk doluysa dispatcher bekler. */
    pthread_cond_t not_empty_b;  /* Kuyruk bossa analyzer worker'lari bekler. */
    log_entry_t entries[];       /* Bu level'a ait log_entry_t kayitlari. */
} region_b_level_t;

/* Region D: Priority source'lardan gelen loglarin aggregator tarafindan ayrica skorlandigi kuyruk. */
typedef struct {
    int head;                        /* Aggregator'in okuyacagi siradaki priority entry. */
    int tail;                        /* Dispatcher'in yazacagi siradaki bos priority slotu. */
    int count;                       /* Priority kuyrugunda bekleyen entry sayisi. */
    int capacity;                    /* entries[] kapasitesi. */
    int dispatcher_done;             /* Dispatcher bitti; aggregator daha fazla priority entry beklememeli. */
    pthread_mutex_t priority_mutex;  /* Region D kuyrugunu koruyan mutex. */
    pthread_cond_t not_full_d;       /* Kuyruk doluysa dispatcher bekler. */
    pthread_cond_t not_empty_d;      /* Kuyruk bossa aggregator bekler. */
    log_entry_t entries[];           /* Priority source entry kopyalari. */
} region_d_t;

typedef struct region_c region_c_t;

/* Komut satiri ve config/filter dosyalarindan gelen tum program ayarlari. */
typedef struct {
    int reader_threads;                       /* Her reader process icindeki dosya okuma thread sayisi. */
    int worker_threads;                       /* Her analyzer process icindeki worker thread sayisi. */
    int capacity_a;                           /* Region A kuyruk kapasitesi. */
    int capacity_b;                           /* Her Region B level kuyrugunun kapasitesi. */
    int capacity_d;                           /* Region D priority kuyruk kapasitesi. */
    int timeout_sec;                          /* Timed wait ve shutdown beklemelerinde kullanilan sure. */
    int num_keywords;                         /* -k ile verilen keyword sayisi. */
    int num_files;                            /* Config dosyasindan okunan log dosyasi sayisi. */
    int num_priority_sources;                 /* Filter dosyasindan okunan priority source sayisi. */
    char config_file[MAX_PATH_LEN];           /* Log dosya path'lerini listeleyen config dosyasi. */
    char filter_file[MAX_PATH_LEN];           /* Priority source listesini tutan filter dosyasi. */
    char output_file[MAX_PATH_LEN];           /* Text rapor cikti dosyasi. */
    char binary_file[MAX_PATH_LEN];           /* Binary checkpoint cikti dosyasi. */
    char keywords[MAX_KEYWORDS][64];          /* Mesajlarda aranacak keyword listesi. */
    char files[MAX_FILES][MAX_PATH_LEN];      /* Reader process'lerin okuyacagi log dosyalari. */
    char priority_sources[MAX_SOURCES][64];   /* Region D'ye de kopyalanacak source adlari. */
} program_options_t;

/* mmap ile olusturulan tum paylasimli bolgelerin adreslerini ve boyutlarini bir arada tutar. */
typedef struct {
    region_a_t *region_a;                    /* Reader -> dispatcher ortak kuyrugu. */
    region_b_level_t *region_b[MAX_LEVELS];  /* Dispatcher -> analyzer level kuyruklari. */
    region_c_t *region_c;                    /* Analyzer -> aggregator sonuc bolgesi. */
    region_d_t *region_d;                    /* Dispatcher -> aggregator priority kuyrugu. */
    size_t region_a_size;                    /* munmap icin saklanan mmap boyutu. */
    size_t region_b_size;                    /* Her Region B mmap boyutu. */
    size_t region_c_size;                    /* Region C mmap boyutu. */
    size_t region_d_size;                    /* Region D mmap boyutu. */
} shared_regions_t;

/* Reader process icindeki thread'ler arasi ozel kuyruk; process disina paylasilmaz. */
typedef struct {
    pthread_mutex_t mutex;       /* Reader ve parser thread'leri arasinda buffer'i korur. */
    pthread_cond_t not_full;     /* Buffer doluysa reader thread'ler bekler. */
    pthread_cond_t not_empty;    /* Buffer bossa parser thread bekler. */
    int head;                    /* Parser'in okuyacagi siradaki entry. */
    int tail;                    /* Reader thread'in yazacagi siradaki bos slot. */
    int count;                   /* Buffer'daki entry sayisi. */
    int capacity;                /* entries heap dizisinin kapasitesi. */
    int producers_done;          /* Reader thread'ler bitince parser'in cikmasi icin 1 olur. */
    log_entry_t *entries;        /* Process-local log_entry_t buffer'i. */
} private_buffer_t;

/* Her child process'e verilecek arguman paketleri. */
typedef struct {
    program_options_t *opts;     /* Global program ayarlari. */
    shared_regions_t *shared;    /* Fork ile miras alinan shared memory adresleri. */
    int reader_id;               /* Bu reader'in okuyacagi files[] indexi. */
    const char *file_path;       /* Okunacak log dosyasinin path'i. */
    int heartbeat_fd;            /* Watchdog'a progress yazilan pipe write end'i. */
} reader_process_args_t;

typedef struct {
    program_options_t *opts;     /* Dispatcher'in kapasite/filter/timeout ayarlari. */
    shared_regions_t *shared;    /* Region A/B/D adresleri. */
} dispatcher_process_args_t;

typedef struct {
    program_options_t *opts;     /* Worker thread sayisi ve keyword listesi. */
    shared_regions_t *shared;    /* Region B ve Region C adresleri. */
    int level_index;             /* Bu analyzer'in sorumlu oldugu ERROR/WARN/INFO/DEBUG indexi. */
} analyzer_process_args_t;

typedef struct {
    program_options_t *opts;     /* Output dosya adlari ve keyword listesi. */
    shared_regions_t *shared;    /* Region C ve Region D adresleri. */
} aggregator_process_args_t;

typedef struct {
    program_options_t *opts;                    /* Reader sayisi ve timeout gibi ayarlar. */
    pid_t *child_pids;                          /* Parent'in takip ettigi child PID listesi. */
    int child_count;                            /* PID listesindeki eleman sayisi. */
    int *pipe_fds;                              /* Reader heartbeat pipe read end'leri. */
    volatile sig_atomic_t *shutdown_flag;       /* Parent set edince watchdog dongusu biter. */
} watchdog_args_t;

extern volatile sig_atomic_t g_sigint_received;

#ifdef __linux__
/* Linux'ta gercek POSIX semaphore kullanilir. */
typedef sem_t hw_sem_t;

static inline int hw_sem_init(hw_sem_t *sem, int pshared, unsigned value) {
    return sem_init(sem, pshared, value);
}

static inline int hw_sem_post(hw_sem_t *sem) {
    return sem_post(sem);
}

static inline int hw_sem_wait(hw_sem_t *sem) {
    return sem_wait(sem);
}

static inline int hw_sem_destroy(hw_sem_t *sem) {
    return sem_destroy(sem);
}
#else
/* macOS gibi ortamlarda process-shared sem_t yerine mutex+cond ile basit semaphore taklidi. */
typedef struct hw_sem {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned count;
} hw_sem_t;

static inline int hw_sem_init(hw_sem_t *sem, int pshared, unsigned value) {
    (void)pshared;
    pthread_mutex_init(&sem->mutex, NULL);
    pthread_cond_init(&sem->cond, NULL);
    sem->count = value;
    return 0;
}

static inline int hw_sem_post(hw_sem_t *sem) {
    pthread_mutex_lock(&sem->mutex);
    sem->count++;
    pthread_cond_signal(&sem->cond);
    pthread_mutex_unlock(&sem->mutex);
    return 0;
}

static inline int hw_sem_wait(hw_sem_t *sem) {
    pthread_mutex_lock(&sem->mutex);
    while (sem->count == 0) {
        pthread_cond_wait(&sem->cond, &sem->mutex);
    }
    sem->count--;
    pthread_mutex_unlock(&sem->mutex);
    return 0;
}

static inline int hw_sem_destroy(hw_sem_t *sem) {
    pthread_mutex_destroy(&sem->mutex);
    pthread_cond_destroy(&sem->cond);
    return 0;
}
#endif

/* Region C: Analyzer sonuclari ve aggregator'in okuyacagi final istatistikler. */
typedef struct region_c {
    level_result_t results[MAX_LEVELS];  /* ERROR/WARN/INFO/DEBUG icin dort sonuc slotu. */
    hw_sem_t level_ready[MAX_LEVELS];    /* Her analyzer sonucunu teslim edince post eder. */
    pthread_mutex_t result_mutex;        /* results ve ready bayraklarini korur. */
    pthread_cond_t result_cond;          /* Aggregator ready degisikligini bununla bekler. */
    double high_priority_score;          /* Priority source loglarindan gelen toplam skor. */
    long high_priority_entries;          /* Priority kuyrugundan islenen entry sayisi. */
} region_c_t;

#ifndef __linux__
/* Linux disi sistemlerde pthread_barrier yoksa basit barrier implementasyonu. */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned count;
    unsigned trip_count;
    unsigned generation;
} pthread_barrier_t;

static inline int pthread_barrier_init(pthread_barrier_t *barrier,
                                       const void *attr,
                                       unsigned count) {
    (void)attr;
    if (count == 0) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_init(&barrier->mutex, NULL);
    pthread_cond_init(&barrier->cond, NULL);
    barrier->count = 0;
    barrier->trip_count = count;
    barrier->generation = 0;
    return 0;
}

static inline int pthread_barrier_wait(pthread_barrier_t *barrier) {
    unsigned generation;
    pthread_mutex_lock(&barrier->mutex);
    generation = barrier->generation;
    barrier->count++;
    if (barrier->count == barrier->trip_count) {
        barrier->generation++;
        barrier->count = 0;
        pthread_cond_broadcast(&barrier->cond);
        pthread_mutex_unlock(&barrier->mutex);
        return 1;
    }
    while (generation == barrier->generation) {
        pthread_cond_wait(&barrier->cond, &barrier->mutex);
    }
    pthread_mutex_unlock(&barrier->mutex);
    return 0;
}

static inline int pthread_barrier_destroy(pthread_barrier_t *barrier) {
    pthread_mutex_destroy(&barrier->mutex);
    pthread_cond_destroy(&barrier->cond);
    return 0;
}
#endif

static inline pid_t get_system_tid(void) {
#ifdef __linux__
    return (pid_t)syscall(SYS_gettid);
#else
    return (pid_t)(uintptr_t)pthread_self();
#endif
}

const char *level_name(int level);
int level_weight(int level);
int parse_level(const char *level_str);
int parse_log_line(const char *line, log_entry_t *entry);
long count_overlapping_keyword(const char *haystack, const char *needle);
void trim_newline(char *s);
void die_errno(const char *msg);
void die_message(const char *msg);
int timed_wait_seconds(struct timespec *ts, int timeout_sec);

#endif
#ifdef __linux__
#include <semaphore.h>
#endif
