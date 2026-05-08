# SystemPrograming-Homeworks

## HW4 - Log Analyzer Sistemi

HW4'te temel olarak cok process'li ve cok thread'li bir log analiz sistemi
gelistirilmistir. Program birden fazla log dosyasini okur, log satirlarini
seviyelerine gore ayirir, verilen keyword'leri arar ve agirlikli skorlar
hesaplayarak text ve binary cikti uretir.

Sistemde ana amac sadece log analizi yapmak degil; process, thread, shared
memory, mutex, condition variable, semaphore, barrier ve pipe gibi sistem
programlama konularini birlikte kullanmaktir.

### main.c

`main.c` programin baslangic ve koordinasyon dosyasidir.

Bu dosyada komut satiri argumanlari okunur. Config dosyasindan analiz edilecek
log dosyalari, filter dosyasindan ise high-priority source listesi yuklenir.
Ardindan shared memory bolgeleri olusturulur ve sistemde kullanilacak child
process'ler fork edilir.

`main.c` tarafindan olusturulan ana process'ler sunlardir:

- Reader process'leri
- Dispatcher process
- Her log seviyesi icin Analyzer process'leri
- Aggregator process

Ayrica parent process icinde bir watchdog thread baslatilir. Bu thread reader
process'lerden pipe uzerinden gelen ilerleme mesajlarini takip eder.

Program calisirken parent process child process'lerin bitmesini `waitpid` ile
bekler. Eger `SIGINT` gelirse child process'lere once `SIGTERM`, gerekirse
`SIGKILL` gondererek sistemi kontrollu sekilde sonlandirir. En sonda Region C
icindeki sonuclari okuyup genel sistem ozetini ekrana yazdirir ve shared memory
bolgelerini temizler.

### common.c

`common.c` ortak yardimci fonksiyonlari icerir. Birden fazla dosyada
kullanilan genel islemler burada toplanmistir.

Bu dosyada log seviyeleri ile ilgili fonksiyonlar bulunur. `level_name`
fonksiyonu level index'ini `ERROR`, `WARN`, `INFO`, `DEBUG` string'ine cevirir.
`level_weight` fonksiyonu her seviyenin skor agirligini verir. Bu agirliklar
su sekildedir:

- `ERROR`: 4
- `WARN`: 3
- `INFO`: 2
- `DEBUG`: 1

`parse_level` string olarak gelen log seviyesini enum degerine cevirir.
`parse_log_line` ise bir log satirini parcalar. Beklenen log formati genel
olarak su sekildedir:

```text
[timestamp] [LEVEL] [source] message
```

Satir bu formata uymuyorsa gecersiz sayilir. Source alaninda sadece alfanumerik
karakterlere izin verilir.

`count_overlapping_keyword` fonksiyonu mesaj icinde keyword gecislerini sayar.
Bu fonksiyon overlapping eslesmeleri de sayar. Ornegin `aaa` icinde `aa`
keyword'u iki kez bulunur.

Dosyada ayrica hata durumlari icin `die_errno`, `die_message`, string temizleme
icin `trim_newline` ve timed wait icin `timed_wait_seconds` fonksiyonlari vardir.

### dispatcher.c

`dispatcher.c` Reader tarafindan Region A'ya konulan log entry'lerini alir ve
log seviyelerine gore ilgili Region B buffer'larina yonlendirir.

Dispatcher bir consumer gibi Region A'dan veri okur. Okudugu entry'nin
`level_index` degerine bakarak onu ilgili level buffer'ina yollar:

- ERROR entry'leri ERROR Region B buffer'ina
- WARN entry'leri WARN Region B buffer'ina
- INFO entry'leri INFO Region B buffer'ina
- DEBUG entry'leri DEBUG Region B buffer'ina

Bu islem sirasinda producer-consumer yapisi kullanilir. Region A ve Region B
buffer'lari mutex ve condition variable ile korunur. Buffer bosken consumer
bekler, buffer doluyken producer bekler.

Dispatcher ayrica source alanini filter dosyasindan gelen high-priority source
listesi ile karsilastirir. Eger log entry'si high-priority source'a aitse entry
Region D'ye de kopyalanir. Region D daha sonra Aggregator tarafindan
high-priority skor hesaplamak icin kullanilir.

Reader process'leri isleri bitince her level icin EOF marker gonderir.
Dispatcher bu EOF marker'lari sayarak her level icin butun reader'larin
bittigini anlar. Bir level tamamen bittiginde ilgili Region B buffer'inda
`eof_posted` isaretlenir ve analyzer process'leri uyandirilir.

### analyzer.c

`analyzer.c` log seviyelerine gore ayrilmis verileri analiz eden dosyadir.
Programda her log seviyesi icin ayri bir analyzer process calisir:

- ERROR analyzer
- WARN analyzer
- INFO analyzer
- DEBUG analyzer

Her analyzer process kendi icinde birden fazla worker thread olusturur. Bu
worker thread'ler ilgili Region B buffer'indan log entry'leri ceker ve mesaj
icinde keyword aramasi yapar.

Skor hesabi su mantikla yapilir:

```text
weighted_score = keyword_match_count * level_weight
```

Ornegin ERROR seviyesinde agirlik 4 oldugu icin bir keyword 3 kez bulunursa
skor `3 * 4 = 12` olur.

Worker thread'ler kendi ara sonuclarini Thread Local Storage kullanarak tutar.
Thread biterken TLS destructor calisir ve thread'in hesapladigi skorlar Region C
icindeki ortak sonuc alanina eklenir. Bu ekleme islemi `result_mutex` ile
korunur.

Analyzer icinde `pthread_barrier_t` de kullanilir. Bu sayede tum worker
thread'ler bitmeden raporlama asamasina gecilmez. Son olarak analyzer kendi
level sonucunu hazir olarak isaretler, condition variable ile Aggregator'i
uyandirir ve `level_ready` semaphore'una post eder.

Analyzer ayrica her level icin en yuksek skora sahip top-3 source bilgisini ve
thread bazli skor katkilarini da hesaplar.

### aggregator.c

`aggregator.c` sistemin sonuc toplama ve cikti uretme dosyasidir.

Aggregator once Region D uzerinden high-priority entry'leri okur. Bu entry'ler
filter dosyasinda belirtilen source'lardan gelen loglardir. Aggregator bu
entry'ler icin de keyword skorlarini hesaplar ve Region C icindeki
`high_priority_score` alanina ekler.

Daha sonra Aggregator, Analyzer process'lerinin sonuclarini bekler. Her level
icin Region C'deki `ready` bayragini, condition variable'i ve `level_ready`
semaphore'unu kullanarak sonuc hazir olana kadar bekler.

Tum level sonuclari hazir oldugunda iki cikti dosyasi olusturulur:

- Text output dosyasi
- Binary checkpoint dosyasi

Text output dosyasinda keyword listesi, toplam agirlikli skor, high-priority
skor, level bazli skorlar, keyword bazli skorlar, top-3 source bilgileri ve
thread bazli katkilar yazilir.

Binary checkpoint dosyasinda once `checkpoint_header_t` header'i, sonra her
level icin `level_result_t` yapilari yazilir. Binary dosya once `.tmp` dosyasina
yazilir, sonra `rename` ile asil dosya haline getirilir. Bu yontem yazma islemi
yarida kalirsa asil checkpoint dosyasinin bozulmasini engeller.

### HW4 Sozlu / Soru-Cevap Notlari

Bu bolum HW4 kodu uzerinden gelebilecek klasik C, process, thread ve IPC
sorularina kisa cevaplar verir.

#### General C and File/Stream Operations

**`memset`, `strcmp` ve `memcpy` ne yapar?**

- `memset(ptr, value, size)` verilen bellek bolgesini byte byte ayni degerle
  doldurur. HW4'te struct'lari sifirlamak icin kullanilir; ornegin
  `memset(opts, 0, sizeof(*opts))`.
- `strcmp(a, b)` iki C string'ini karsilastirir. Sonuc `0` ise string'ler
  aynidir. HW4'te source veya level karsilastirmalarinda kullanilir.
- `memcpy(dst, src, size)` ham byte kopyalar. String sonuna otomatik `\0`
  koymaz; bu yuzden HW4'te timestamp/level/source kopyalandiktan sonra manuel
  null terminator eklenir.

**`open` ile `fopen` arasindaki fark nedir, ne dondururler?**

`open` POSIX sistem cagrisidir ve basarili olursa integer file descriptor
dondurur; hata durumunda `-1` dondurur ve `errno` set edilir. `read`, `write`,
`close` gibi dusuk seviyeli fonksiyonlarla kullanilir. `fopen` ise C standard
I/O fonksiyonudur, basarili olursa `FILE *` stream dondurur, hata durumunda
`NULL` dondurur. `fgets`, `getline`, `fprintf`, `fclose` gibi buffered stdio
fonksiyonlariyla kullanilir.

**`fgets` ile `read` arasindaki fark nedir?**

`fgets(buffer, size, fp)` bir `FILE *` stream'den satir okur; newline'a,
EOF'a veya `size - 1` karaktere kadar okur ve string sonuna `\0` koyar.
`read(fd, buf, n)` file descriptor'dan en fazla `n` byte okur; satir kavrami
yoktur ve buffer'i C string yapmak icin otomatik `\0` eklemez. HW4'te config
ve filter dosyalari `fgets` ile, watchdog pipe mesajlari ise `read` ile okunur.

**`fflush` ne yapar, bu projede neyi yazar?**

`fflush(stream)` stdio output buffer'inda bekleyen veriyi alttaki dosyaya veya
terminal/pipe gibi hedefe zorla yazar. Bu projede `main.c` icinde `fork`
oncesi `fflush(stdout)` cagirilir; amac parent'in bastigi "Forking ..."
satirlarinin stdout buffer'inda kalip child process'ler tarafindan tekrar
yazilmasini engellemektir. `analyzer.c` icinde dogrudan `fflush` yoktur;
oradaki `printf` ciktilari stdout'a gider ve `main.c` basinda ayarlanan line
buffering fork sonrasi child process'lere de miras kalir.

**`dispatcher.c` icinde `close()` kullanildiysa dosya descriptor'u neyle acilmistir?**

`close()` bir file descriptor kapatir; bu descriptor `open`, `pipe`, `socket`,
`dup` veya benzeri dusuk seviyeli bir mekanizmadan gelmis olabilir. HW4'te
pipe descriptor'lari `pipe(pipe_fds[i])` ile olusturulur ve sonra `close(...)`
ile kapatilir. `fopen` ile acilan stream'ler icin `close` degil `fclose`
kullanilir.

#### Command-Line Parsing

**`getopt` ne icin kullanilir?**

`getopt`, `argv` icindeki `-c`, `-f`, `-k`, `-t`, `-w`, `-a`, `-b`, `-d`,
`-T`, `-o`, `-O` gibi komut satiri seceneklerini sirayla parse etmek icin
kullanilir. HW4'te parent process tum komut satiri argumanlarini `parse_args`
icinde okur, `program_options_t` yapisina yazar ve zorunlu alanlari valide eder.

#### Process and Thread Lifecycle

**`fork()` hangi degerleri dondurur, parent ne alir?**

`fork()` basarili olursa iki process olusur. Child process icinde donus degeri
`0` olur. Parent process icinde donus degeri child'in PID degeridir. Hata
olursa parent tarafinda `-1` dondurur. HW4'te parent bu PID'leri `children.pids`
dizisine ekler ve sonra `waitpid`/`kill` icin kullanir.

**`waitpid` ucuncu parametresi ne yapar? `0` ile `WNOHANG` farki nedir?**

`waitpid(pid, &status, options)` icindeki ucuncu parametre bekleme davranisini
belirler. `0` verilirse cagri blocking olur; uygun child bitene kadar bekler.
`WNOHANG` verilirse blocking olmaz; bitmis child yoksa hemen `0` dondurur.
HW4'te normal akis `waitpid(-1, &status, 0)` ile child bitmesini bekler.
SIGINT/shutdown sirasinda ise `waitpid(-1, &status, WNOHANG)` ile child'larin
bitip bitmedigi polling mantigiyla kontrol edilir.

**`pthread_attr_t` nedir?**

`pthread_attr_t`, thread olusturulurken stack size, detach state, scheduling
policy gibi attribute'lari tasiyan tiptir. `pthread_create` ikinci parametre
olarak bu attribute'u alabilir. HW4'te thread'ler default attribute ile
olusturuldugu icin ikinci parametre `NULL` verilir; yani ozel bir
`pthread_attr_t` kullanilmiyor.

**`pthread_create` basarisiz olursa `pthread_join` nasil davranir?**

`pthread_create` basarisiz olursa gecerli bir thread olusmaz. Bu durumda o
thread id uzerinde `pthread_join` cagirmak dogru degildir; hata dondurebilir
ve davranis guvenilir kabul edilmez. Ideal olarak `pthread_create` donus kodu
kontrol edilmeli ve sadece basariyla olusan thread'ler join edilmelidir.

#### Pipes and IPC

**Pipe nedir, ne icin kullanilir?**

`pipe(fd)` tek yonlu bir kernel buffer'i olusturur ve iki file descriptor
verir: `fd[0]` okuma ucu, `fd[1]` yazma ucu. HW4'te parent her reader icin
bir pipe cift olusturur. Reader process heartbeat mesajlarini write end'e
yazar, parent icindeki watchdog thread read end'i `select` ve `read` ile takip
eder.

#### Circular Buffers and Data Structures

**`reader.c` icinde `head` ve `tail` ne yapar?**

`head`, circular buffer'da okunacak siradaki elemanin indexidir. `tail`,
yazilacak siradaki bos slotun indexidir. Eleman eklenince `tail = (tail + 1) %
capacity`, eleman okununca `head = (head + 1) % capacity` yapilir. HW4'te bu
mantik reader private buffer'inda, Region A'da, Region B'de ve Region D'de
kullanilir.

**`head` ve `tail` ayni mutex altinda mi guncelleniyor?**

Evet. Her circular buffer kendi mutex'i altinda guncellenir. Reader private
buffer icin `buffer.mutex`, Region A icin `input_mutex`, Region B icin
`level_mutex`, Region D icin `priority_mutex` kullanilir. Boylece `head`,
`tail` ve `count` birlikte tutarli kalir.

**`head == tail` ise buffer kesin bos mudur?**

Her implementasyonda kesin bos denemez. Sadece `head` ve `tail` varsa
`head == tail` hem bos hem de tamamen dolu durum anlamina gelebilir. HW4 bu
belirsizligi `count` alaniyla cozer: `count == 0` bos, `count == capacity`
dolu demektir.

#### Synchronization Primitives

**Mutex nedir, critical region nedir? HW4'te nerelerde var?**

Mutex ayni anda sadece bir thread veya process'in belirli ortak veriye
girmesini saglayan kilittir. Critical region, paylasilan verinin okundugu veya
degistirildigi ve bu yuzden mutex ile korunmasi gereken kod parcasidir. HW4'te
Region A/B/D kuyruk push-pop islemleri, Region C sonuc guncellemeleri ve
analyzer source hit dizileri critical region'dir.

**`sem_post` ne yapar?**

`sem_post`, semaphore sayacini 1 artirir ve semaphore'da bekleyen bir
`sem_wait` varsa onu uyandirabilir. HW4'te wrapper adi `hw_sem_post` olarak
kullanilir. Analyzer bir level sonucunu hazir edince
`level_ready[level_index]` semaphore'una post eder; aggregator da
`hw_sem_wait` ile bu sinyali bekler.

**`sem_init` icinde `pshared` neden 1 verilir?**

`pshared = 1`, semaphore'un process'ler arasinda paylasilacagini belirtir.
HW4'te semaphore Region C icindeki shared memory alaninda durur ve analyzer
process'i ile aggregator process'i arasinda kullanilir. Bu yuzden Linux
tarafinda `sem_init(&sem, 1, 0)` mantigi gerekir.

**Neden `pthread_cond_broadcast` kullanildi, ne yapar?**

`pthread_cond_broadcast`, ayni condition variable uzerinde bekleyen tum
thread/process'leri uyandirir. HW4'te EOF geldiginde bir Region B level
kuyrugunda birden fazla analyzer worker bekliyor olabilir; sadece birini
uyandirmak yeterli olmayabilir. Bu yuzden `dispatcher.c` EOF sirasinda
`not_empty_b` icin broadcast yapar. Benzer sekilde analyzer sonucu hazir
olunca Region C `result_cond` icin broadcast yapilir.

#### Signals

**`SIGCHLD` ve `SIGINT` nasil calisir?**

`SIGCHLD`, bir child process durdugunda veya bittiginde parent'a gonderilen
sinyaldir; parent genelde `wait`/`waitpid` ile child'i reap eder. HW4'te
explicit bir `SIGCHLD` handler yoktur; parent child'lari `waitpid` ile bekler.
`SIGINT` genelde Ctrl+C ile gelir. HW4'te `SIGINT` handler'i sadece global
`g_sigint_received` bayragini set eder; asil temiz kapatma main loop icinde
yapilir.

**`SIGTERM` ile `SIGKILL` farki nedir?**

`SIGTERM` process'e kibar kapatma istegidir; process bunu yakalayabilir,
ignore edebilir veya cleanup yapabilir. `SIGKILL` yakalanamaz ve ignore
edilemez; kernel process'i zorla sonlandirir. HW4 shutdown akisi once
`SIGTERM`, sure dolarsa `SIGKILL` gonderir.

**Bir process'e `SIGTERM` gelirse tum thread'lere yayilir mi?**

Sinyal process'e gonderildiginde process icindeki thread'lerden biri tarafindan
teslim alinabilir; sinyal disposition'i process genelindedir. `SIGTERM` icin
default davranis tum process'i sonlandirmaktir, dolayisiyla o process icindeki
butun thread'ler de biter.

#### Memory Management

**`mmap` nedir, nasil calisir?**

`mmap`, bir dosyayi veya anonim bellek bolgesini process adres alanina map
eder. HW4'te `MAP_SHARED | MAP_ANONYMOUS` ile dosyaya bagli olmayan ama
fork'tan sonra child process'lerle paylasilan memory region'lari olusturulur.
Region A/B/C/D bu sekilde kurulur; iclerindeki mutex/condition/semaphore
nesneleri de process-shared olacak sekilde init edilir.

#### Project-Specific Logic and Formatting

**Dispatcher routing nasil calisir? Barrier neye gore kontrol edilir?**

Reader'lar parse ettikleri loglari once Region A'ya koyar. Dispatcher Region
A'dan alir, `entry.level_index` degerine gore ilgili Region B level kuyruguna
yazar. Source filter listesinde varsa ayni entry Region D'ye de kopyalanir.
Reader'lar bitince her level icin EOF marker yollar; dispatcher her level icin
`num_files` kadar EOF gorunce o Region B'yi `eof_posted = 1` yapar ve
analyzer worker'larini uyandirir.

Analyzer tarafindaki barrier `pthread_barrier_init(&barrier, NULL,
worker_threads)` ile kurulur. Yani beklenen counter, komut satirindan gelen
`-w` worker thread sayisidir. Her worker `pthread_barrier_wait` noktasina
gelmeden analyzer process reporting/cleanup asamasina gecmez.

**Neden belirli yerde `fprintf` yerine `printf`, veya `printf` yerine `fprintf` kullanildi?**

`printf` her zaman stdout'a yazar ve ekrandaki runtime loglari icin uygundur.
`fprintf(fp, ...)` secilen stream'e yazar; bu bir dosya veya `stderr` olabilir.
HW4'te aggregator raporu output dosyasina yazmak icin `fprintf(fp, ...)`
kullanir. Watchdog progress bilgisini asil program output'unu kirletmemek icin
`fprintf(stderr, ...)` ile stderr'e yazar.

**`while` yerine `for` ile sonsuz dongu nasil yazilir?**

`while (1) { ... }` yerine ayni mantik `for (;;) { ... }` ile yazilir. Iki
ifade de sonsuz dongudur; donguden cikmak icin `break`, `return` veya process
sonlandirma gerekir.

**Makefile'daki `-Wall` flag'i ne yapar?**

`-Wall`, GCC'nin yaygin uyari kontrollerini acmasini saglar. Hatalari tek
basina engellemez ama kullanilmayan degisken, supheli karsilastirma, eksik
return gibi sorunlari derleme sirasinda gorunur yapar. HW4 Makefile ayrica
`-Wextra`, `-std=c11` ve `-pthread` kullanir.

### HW4 Ek 15 Soru-Cevap

**1. Region A, Region B, Region C ve Region D hangi amacla kullaniliyor?**

Region A reader process'lerinden dispatcher'a giden ortak input kuyrugudur.
Region B dispatcher'dan analyzer process'lerine giden level bazli kuyruklardir;
ERROR, WARN, INFO ve DEBUG icin ayri Region B vardir. Region C analyzer
sonuclarinin toplandigi shared result alanidir. Region D ise filter dosyasinda
belirtilen high-priority source'lardan gelen entry'lerin aggregator tarafindan
ayrica skorlanmasi icin kullanilir.

**2. Reader process ile reader thread arasindaki fark nedir?**

Her log dosyasi icin bir reader process fork edilir. Bu process kendi icinde
birden fazla reader thread olusturur ve dosyayi byte araliklarina bolerek okur.
Yani process seviyesi dosya bazli paralellik, thread seviyesi ise ayni dosya
icinde paralel okuma saglar.

**3. Reader thread dosyanin ortasindan baslarsa yarim satir problemi nasil cozuluyor?**

Thread'in `start_offset` degeri 0'dan buyukse once bir onceki karakter kontrol
edilir. Eger bu karakter newline degilse thread ilk newline karakterine kadar
ilerler. Boylece dosyanin ortasindan baslayan thread yarim log satirini parse
etmeye calismaz.

**4. Reader neden dogrudan Region A'ya yazmak yerine private buffer kullaniyor?**

Reader thread'leri dosyadan satir okuyup parse ederken parser thread bu
entry'leri Region A'ya aktarir. Aradaki private buffer, ayni reader process
icinde producer-consumer modeli kurar. Boylece dosya okuma/parsing isi ile
shared memory'ye yazma isi birbirinden ayrilir.

**5. EOF marker neden her level icin ayri gonderiliyor?**

Analyzer process'leri level bazli calistigi icin dispatcher'in her level icin
ayri bitis bilgisine ihtiyaci vardir. Bir reader dosyayi bitirdiginde ERROR,
WARN, INFO ve DEBUG icin EOF marker yollar. Dispatcher bir level icin
`num_files` kadar EOF gorunce o level'in Region B kuyrugunda `eof_posted`
bayragini set eder.

**6. Dispatcher bir entry'nin hangi analyzer'a gidecegine nasil karar veriyor?**

Dispatcher `entry.level_index` alanina bakar. Bu alan `parse_log_line` icinde
`parse_level` ile uretilir. Level ERROR ise ERROR Region B'ye, WARN ise WARN
Region B'ye, INFO ise INFO Region B'ye, DEBUG ise DEBUG Region B'ye yazilir.

**7. High-priority source mantigi normal analizden ayri mi calisiyor?**

High-priority entry normal akistan cikmaz; once kendi level'ina gore Region
B'ye gider. Ek olarak source filter listesinde varsa Region D'ye de kopyalanir.
Bu nedenle ayni log hem genel level skoruna dahil olur hem de
`high_priority_score` hesabina katkida bulunur.

**8. `count_overlapping_keyword` neden normal `strstr` dongusunden farkli?**

Bu fonksiyon cakisan eslesmeleri de sayar. Ornegin mesaj `aaa`, keyword `aa`
ise sonuc 2 olur. Normal aramayi eslesmeden sonra keyword uzunlugu kadar
ilerleterek yapsaydik sonuc 1 olurdu. HW4'te skor hesabi bu daha hassas sayima
gore yapilir.

**9. Weighted score nasil hesaplanir?**

Her keyword eslesme sayisi log level agirligi ile carpilir. Agirliklar ERROR
icin 4, WARN icin 3, INFO icin 2, DEBUG icin 1'dir. Ornegin WARN seviyesinde
bir keyword 5 kez gecerse skor `5 * 3 = 15` olur.

**10. Analyzer'da Thread Local Storage neden kullanildi?**

Her worker thread kendi keyword skorlarini lokal olarak tutar. Thread bitince
TLS destructor calisir ve bu lokal skorlar Region C'deki ortak sonuca
`result_mutex` altinda eklenir. Bu tasarim worker'larin her entry icin shared
result mutex'ine girmesini azaltir.

**11. TLS destructor tam olarak ne zaman calisir?**

Worker thread `pthread_exit(NULL)` ile bittiginde, `pthread_key_create` ile
tanimlanan destructor o thread'e ait TLS verisi icin cagrilir. HW4'te
`tls_destructor`, thread'in per-keyword ve per-thread skorlarini Region C'ye
aktarir ve ayirdigi payload bellegini free eder.

**12. Aggregator analyzer sonuclarinin hazir oldugunu nasil anliyor?**

Analyzer once Region C icindeki ilgili `result->ready` bayragini 1 yapar ve
`result_cond` ile bekleyenleri uyandirir. Sonra `level_ready[level]`
semaphore'una `hw_sem_post` yapar. Aggregator hem ready bayragini condition
variable ile bekler hem de semaphore uzerinden sonucu teslim aldigini
dogrular.

**13. Binary checkpoint neden once `.tmp` dosyasina yaziliyor?**

Program dogrudan asil binary dosyaya yazarken yarida kesilirse bozuk veya eksik
checkpoint kalabilir. Bu yuzden once `output.tmp` gibi gecici dosyaya header ve
level sonuclari yazilir, yazma basarili olursa `rename` ile asil dosya adina
tasinar. Bu daha atomik ve guvenli bir cikti uretme yontemidir.

**14. Watchdog thread neyi takip ediyor, neden parent icinde calisiyor?**

Watchdog parent process icinde calisir ve reader process'lerin pipe uzerinden
gonderdigi heartbeat mesajlarini izler. `select` ile pipe read end'lerini
bekler, gelen mesajlardan reader bazli ilerleme bilgisini cikarir ve belirli
araliklarla stderr'e progress raporu yazar. Parent icinde olmasi tum child
PID'lerine ve pipe read end'lerine merkezi olarak erismeyi kolaylastirir.

**15. SIGINT gelince sistem nasil kontrollu kapanir?**

Ctrl+C ile `SIGINT` geldiginde signal handler sadece
`g_sigint_received` bayragini set eder. Parent `waitpid` sirasinda `EINTR`
alip bu bayragi gorurse `terminate_children` fonksiyonunu cagirir. Bu fonksiyon
once child process'lere `SIGTERM` yollar, belirli sure `WNOHANG` ile bitmelerini
bekler, hala yasayanlara `SIGKILL` yollar ve sonunda shared memory alanlarini
temizler.

### HW4 C Dosyalari Fonksiyon Ozetleri

#### main.c

- `mark_child_reaped`: Biten child process'i listede isaretler, boylece tekrar kapatilmaya calisilmaz.
- `terminate_children`: Program kapanirken child process'leri once nazikce, gerekirse zorla kapatir.
- `sigint_handler`: Ctrl+C gelince sadece `g_sigint_received` bayragini 1 yapar.
- `install_sigint_handler`: Ctrl+C yakalansin diye parent process'e `SIGINT` handler baglar.
- `usage`: Programin nasil calistirilmasi gerektigini ekrana hata mesaji olarak yazar.
- `parse_keywords`: `-k` ile gelen keyword'leri virgullerden ayirip listeye koyar.
- `load_file_paths`: Config dosyasindaki log file path'lerini okuyup diziye doldurur.
- `load_sources_simple`: Filter dosyasindaki high-priority source isimlerini okuyup listeye koyar.
- `parse_args`: Komut satiri argumanlarini okur, kontrol eder ve gerekli dosya listelerini yukler.
- `fork_reader`: Bir log dosyasini okuyacak yeni reader process'i baslatir.
- `fork_dispatcher`: Loglari Region A'dan alip ilgili yerlere dagitacak dispatcher process'i baslatir.
- `fork_analyzer`: Belirli bir log level'i icin analyzer process'i baslatir.
- `fork_aggregator`: Sonuclari toplayip output dosyalarini yazacak aggregator process'i baslatir.
- `main`: Programin butun kurulumunu yapar, process'leri baslatir, bitmelerini bekler ve final summary basar.

#### common.c

- `level_name`: Level numarasini `ERROR`, `WARN`, `INFO` veya `DEBUG` yazisina cevirir.
- `level_weight`: Her log level'i icin kullanilacak skor katsayisini verir.
- `parse_level`: Logdaki level yazisini programin kullandigi level index'ine cevirir.
- `trim_newline`: Satirin sonundaki newline karakterlerini siler.
- `parse_log_line`: Bir log satirini timestamp, level, source ve message parcalarina ayirir.
- `count_overlapping_keyword`: Mesaj icinde keyword kac kez geciyor diye sayar ve cakisanlari da dahil eder.
- `die_errno`: Sistem hatasini yazdirir ve programi durdurur.
- `die_message`: Kendi hata mesajimizi yazdirir ve programi durdurur.
- `timed_wait_seconds`: Timed wait icin kac saniye beklenecegini gosteren zaman bilgisini hazirlar.

#### shm.c

- `init_pshared_mutex`: Bir mutex'i process'ler arasinda kullanilacak sekilde hazirlar.
- `init_pshared_cond`: Bir condition variable'i process'ler arasinda kullanilacak sekilde hazirlar.
- `init_shared_regions`: Region A, B, C ve D icin shared memory alanlarini kurar.
- `destroy_shared_regions`: Program sonunda shared memory alanlarini ve iclerindeki sync nesnelerini temizler.

#### reader.c

- `private_buffer_init`: Reader process icindeki gecici buffer'i kullanima hazirlar.
- `private_buffer_destroy`: Reader'in gecici buffer'ini ve ona ait kaynaklari temizler.
- `private_buffer_push`: Reader thread'in buldugu log entry'yi private buffer'a koyar.
- `private_buffer_pop`: Parser thread'in private buffer'dan siradaki log entry'yi almasini saglar.
- `send_heartbeat`: Reader'in ilerleme bilgisini watchdog'a pipe ile gonderir.
- `reader_thread_main`: Reader thread'in dosyanin kendi parcasini okuyup loglari parse ettigi fonksiyondur.
- `push_region_a`: Parser thread'in log entry veya EOF marker'i Region A'ya koymasini saglar.
- `parser_thread_main`: Private buffer'daki loglari Region A'ya tasir ve is bitince EOF marker'lari yollar.
- `run_reader_process`: Reader process'in thread'lerini ve private buffer akisini yonetir.

#### dispatcher.c

- `source_is_priority`: Logun source degeri filter listesinde var mi diye kontrol eder.
- `pop_region_a`: Dispatcher'in Region A'dan siradaki entry'yi almasini saglar.
- `push_region_b`: Dispatcher'in entry'yi dogru level'a ait Region B'ye koymasini saglar.
- `push_region_d`: High-priority olan entry'nin bir kopyasini Region D'ye koyar.
- `run_dispatcher_process`: Dispatcher'in Region A'dan aldigi loglari Region B ve gerekirse Region D'ye dagittigi ana fonksiyondur.

#### analyzer.c

- `tls_destructor`: Worker thread bitince kendi skorlarini Region C'ye ekler ve kendi bellegini temizler.
- `pop_level_entry`: Worker'in kendi level'ina ait Region B'den log almasini saglar.
- `update_source_hits`: Bir source'un toplam skor katkisini gunceller.
- `compute_top3`: En yuksek skoru ureten ilk uc source'u bulur.
- `worker_main`: Worker thread'in loglari okuyup keyword skorlarini hesapladigi ana fonksiyondur.
- `run_analyzer_process`: Analyzer process'in worker thread'leri baslatip sonucu hazir hale getirdigi ana fonksiyondur.

#### aggregator.c

- `cmp_level_desc`: Level'lari skora gore buyukten kucuge siralamak icin kullanilir.
- `aggregate_high_priority`: Region D'deki high-priority loglari okuyup ekstra high-priority skorunu hesaplar.
- `write_text_output`: Final sonucu okunabilir text dosyasina yazar.
- `write_binary_output`: Final sonucu binary checkpoint dosyasina yazar.
- `run_aggregator_process`: Aggregator'in high-priority skoru toplayip output dosyalarini urettigi ana fonksiyondur.

#### watchdog.c

- `count_alive_children`: Child process'lerden kac tanesi hala calisiyor diye sayar.
- `watchdog_thread_main`: Reader'lardan gelen heartbeat mesajlarini takip edip progress bilgisini yazdirir.
