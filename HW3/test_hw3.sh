#!/bin/bash
# =============================================================================
# test_hw3.sh - CSE344 HW3 Kapsamli Test Scripti
# Multi-Process Word Transportation and Concurrent Sorting System
# Debian uzerinde calistirilmak uzere hazirlanmistir.
# =============================================================================

set -u

# ============================================================
# Renkler
# ============================================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# ============================================================
# Konfigurasyon
# ============================================================
BINARY="./hw3"
INPUT="input_big.txt"
RUN_TIMEOUT=60    # normal calistirmalar icin saniye
BIG_TIMEOUT=120   # buyuk input icin saniye
PASS=0
FAIL=0
TOTAL=0
SKIP=0

# ============================================================
# Yardimci fonksiyonlar
# ============================================================
pass() {
    echo -e "  ${GREEN}[PASS]${NC} $1"
    PASS=$((PASS + 1))
    TOTAL=$((TOTAL + 1))
}

fail() {
    echo -e "  ${RED}[FAIL]${NC} $1"
    FAIL=$((FAIL + 1))
    TOTAL=$((TOTAL + 1))
}

skip() {
    echo -e "  ${CYAN}[SKIP]${NC} $1"
    SKIP=$((SKIP + 1))
}

info() {
    echo -e "  ${BLUE}[INFO]${NC} $1"
}

section() {
    echo ""
    echo -e "${YELLOW}================================================================${NC}"
    echo -e "${YELLOW}  $1${NC}"
    echo -e "${YELLOW}================================================================${NC}"
}

# GNU timeout macOS'ta varsayilan olarak gelmez. Bu sarmalayici, scriptteki
# mevcut "timeout <sure> <komut>" kullanimlarini platformlar arasi calisir kilar.
timeout() {
    if type -P gtimeout >/dev/null 2>&1; then
        command gtimeout "$@"
        return $?
    fi

    if type -P timeout >/dev/null 2>&1; then
        command timeout "$@"
        return $?
    fi

    if ! command -v python3 >/dev/null 2>&1; then
        echo "timeout/gtimeout/python3 bulunamadi" >&2
        return 127
    fi

    local duration="$1"
    shift

    python3 - "$duration" "$@" <<'PY'
import os
import signal
import subprocess
import sys

duration = int(sys.argv[1])
cmd = sys.argv[2:]

proc = subprocess.Popen(cmd)

def _terminate(*_args):
    try:
        proc.terminate()
    except ProcessLookupError:
        pass
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        try:
            proc.kill()
        except ProcessLookupError:
            pass
        proc.wait()
    sys.exit(124)

signal.signal(signal.SIGALRM, _terminate)
signal.alarm(duration)

try:
    rc = proc.wait()
finally:
    signal.alarm(0)

if rc < 0:
    sys.exit(128 + (-rc))
sys.exit(rc)
PY
}

# Gecici dosyalari temizle
cleanup_tmp() {
    rm -f /tmp/hw3_test_*.txt /tmp/hw3_out_*.txt /tmp/hw3_sf_*.txt \
          /tmp/hw3_stdout_*.txt /tmp/hw3_err_*.txt \
          /tmp/hw3_single*.txt /tmp/hw3_run*.txt \
          /tmp/hw3_build*.txt /tmp/hw3_shm*.txt \
          /tmp/hw3_zombie*.txt /tmp/hw3_big*.txt \
          /tmp/hw3_min*.txt /tmp/hw3_lc*.txt /tmp/hw3_tc*.txt \
          /tmp/hw3_hf*.txt /tmp/hw3_basic*.txt /tmp/hw3_same*.txt 2>/dev/null
    # Sizinti eden paylasmali bellek segmentlerini temizle
    if command -v ipcrm &>/dev/null && command -v ipcs &>/dev/null; then
        ipcs -m 2>/dev/null | awk 'NR>3 && $1 ~ /^0x/{print $2}' \
            | xargs -r -I{} ipcrm -m {} 2>/dev/null || true
    fi
}

# Program calistirma: cikti/hata yakalanir
run_hw3() {
    local out_file="$1"
    local err_file="$2"
    shift 2
    timeout "$RUN_TIMEOUT" "$BINARY" "$@" > "$out_file" 2> "$err_file"
    return $?
}

# ============================================================
# ============================================================
# ON KONTROL
# ============================================================
# ============================================================
section "On Kontrol"

# Binary mevcut degil -> derlemeyi dene
if [ ! -f "$BINARY" ]; then
    info "Binary bulunamadi, derleniyor..."
    make > /tmp/hw3_build_out.txt 2>&1
    if [ $? -ne 0 ]; then
        echo -e "${RED}DERLEME BASARISIZ - Testler calismayacak${NC}"
        cat /tmp/hw3_build_out.txt
        exit 1
    fi
fi

if [ ! -x "$BINARY" ]; then
    echo -e "${RED}Binary calistirilamaz: $BINARY${NC}"
    exit 1
fi
pass "Binary mevcut ve calistirabilir"

if [ ! -f "$INPUT" ]; then
    echo -e "${RED}$INPUT bulunamadi - Cikan${NC}"
    exit 1
fi
pass "$INPUT mevcut"

# ============================================================
# ============================================================
# TEST GRUBU 1: Derleme
# ============================================================
# ============================================================
section "1. Derleme Testleri"

make clean > /dev/null 2>&1
make > /tmp/hw3_build_out.txt 2>&1
BUILD_CODE=$?

if [ $BUILD_CODE -eq 0 ] && [ -f "$BINARY" ]; then
    pass "make basarili, binary uretildi"
else
    fail "make basarisiz (cikis kodu: $BUILD_CODE)"
    cat /tmp/hw3_build_out.txt
    echo -e "${RED}Binary olmadan testler devam edemez${NC}"
    exit 1
fi

WARN_COUNT=$(grep "warning:" /tmp/hw3_build_out.txt 2>/dev/null | wc -l)
if [ "$WARN_COUNT" -gt 0 ]; then
    info "Derleme $WARN_COUNT uyari uretti (-Wall -Wextra -pedantic)"
else
    pass "Derleyici uyarisi yok"
fi

# ============================================================
# ============================================================
# TEST GRUBU 2: Parametre Dogrulama
# ============================================================
# ============================================================
section "2. Parametre Dogrulama (-f -w -l -s -c -d -r -i -o)"

# 2.1 Hicbir arguman yok
"$BINARY" > /dev/null 2>/tmp/hw3_err.txt
EC=$?
if [ $EC -ne 0 ] && [ -s /tmp/hw3_err.txt ]; then
    pass "Argumansiz: sifirdan buyuk cikis + stderr mesaji"
elif [ $EC -ne 0 ]; then
    pass "Argumansiz: sifirdan buyuk cikis kodu (stderr bos)"
else
    fail "Argumansiz: cikis kodu 0 bekleniyordu != 0"
fi

# 2.2 - 2.10: Her zorunlu parametrenin eksik olmasi durumu
check_missing() {
    local desc="$1"
    shift
    "$BINARY" "$@" > /dev/null 2>/tmp/hw3_err.txt
    local ec=$?
    if [ $ec -ne 0 ]; then
        pass "$desc eksik: sifirdan buyuk cikis"
    else
        fail "$desc eksik: cikis 0 olmamali"
    fi
}

check_missing "-f" -w 2 -l 2 -s 2 -c 5 -d 8 -r 5 -i "$INPUT" -o /tmp/hw3_out_x.txt
check_missing "-w" -f 3 -l 2 -s 2 -c 5 -d 8 -r 5 -i "$INPUT" -o /tmp/hw3_out_x.txt
check_missing "-l" -f 3 -w 2 -s 2 -c 5 -d 8 -r 5 -i "$INPUT" -o /tmp/hw3_out_x.txt
check_missing "-s" -f 3 -w 2 -l 2 -c 5 -d 8 -r 5 -i "$INPUT" -o /tmp/hw3_out_x.txt
check_missing "-c" -f 3 -w 2 -l 2 -s 2 -d 8 -r 5 -i "$INPUT" -o /tmp/hw3_out_x.txt
check_missing "-d" -f 3 -w 2 -l 2 -s 2 -c 5 -r 5 -i "$INPUT" -o /tmp/hw3_out_x.txt
check_missing "-r" -f 3 -w 2 -l 2 -s 2 -c 5 -d 8 -i "$INPUT" -o /tmp/hw3_out_x.txt
check_missing "-i" -f 3 -w 2 -l 2 -s 2 -c 5 -d 8 -r 5 -o /tmp/hw3_out_x.txt
check_missing "-o" -f 3 -w 2 -l 2 -s 2 -c 5 -d 8 -r 5 -i "$INPUT"

# 2.11 - 2.17: Sinir disi degerler (0 veya negatif)
check_invalid() {
    local desc="$1"
    shift
    "$BINARY" "$@" > /dev/null 2>/tmp/hw3_err.txt
    local ec=$?
    if [ $ec -ne 0 ]; then
        pass "$desc: gecersiz deger -> sifirdan buyuk cikis"
    else
        fail "$desc: gecersiz deger kabul edildi (cikis 0)"
    fi
}

check_invalid "num_floors=0"              -f 0 -w 2 -l 2 -s 2 -c 5 -d 8 -r 5 -i "$INPUT" -o /tmp/hw3_out_x.txt
check_invalid "num_floors=-1"             -f -1 -w 2 -l 2 -s 2 -c 5 -d 8 -r 5 -i "$INPUT" -o /tmp/hw3_out_x.txt
check_invalid "word_carriers=0"           -f 3 -w 0 -l 2 -s 2 -c 5 -d 8 -r 5 -i "$INPUT" -o /tmp/hw3_out_x.txt
check_invalid "letter_carriers=0"         -f 3 -w 2 -l 0 -s 2 -c 5 -d 8 -r 5 -i "$INPUT" -o /tmp/hw3_out_x.txt
check_invalid "sorting_processes=0"       -f 3 -w 2 -l 2 -s 0 -c 5 -d 8 -r 5 -i "$INPUT" -o /tmp/hw3_out_x.txt
check_invalid "max_words_per_floor=0"     -f 3 -w 2 -l 2 -s 2 -c 0 -d 8 -r 5 -i "$INPUT" -o /tmp/hw3_out_x.txt
check_invalid "delivery_capacity=0"       -f 3 -w 2 -l 2 -s 2 -c 5 -d 0 -r 5 -i "$INPUT" -o /tmp/hw3_out_x.txt
check_invalid "reposition_capacity=0"     -f 3 -w 2 -l 2 -s 2 -c 5 -d 8 -r 0 -i "$INPUT" -o /tmp/hw3_out_x.txt

# 2.18: Var olmayan input dosyasi
"$BINARY" -f 3 -w 2 -l 2 -s 2 -c 5 -d 8 -r 5 \
    -i /tmp/dosya_yok_xyz123.txt -o /tmp/hw3_out_x.txt > /dev/null 2>/tmp/hw3_err.txt
EC=$?
if [ $EC -ne 0 ]; then
    pass "Var olmayan input dosyasi: sifirdan buyuk cikis"
else
    fail "Var olmayan input dosyasi: cikis 0 olmamali"
fi

# 2.19: Hata mesajlari stderr'e gitmeli
"$BINARY" -f 0 -w 2 -l 2 -s 2 -c 5 -d 8 -r 5 \
    -i "$INPUT" -o /tmp/hw3_out_x.txt > /tmp/hw3_stdout_v.txt 2>/tmp/hw3_err_v.txt
STDERR_SZ=$(wc -c < /tmp/hw3_err_v.txt)
if [ "$STDERR_SZ" -gt 0 ]; then
    pass "Gecersiz parametre: hata mesaji stderr'e yazildi"
else
    fail "Gecersiz parametre: stderr bos (hata mesaji bekleniyor)"
fi

# 2.20: Sorting_floor input floor sayisinin disinda
cat > /tmp/hw3_bad_floor.txt << 'EOF'
501 test 99
EOF
"$BINARY" -f 3 -w 2 -l 2 -s 2 -c 5 -d 8 -r 5 \
    -i /tmp/hw3_bad_floor.txt -o /tmp/hw3_out_x.txt > /dev/null 2>/tmp/hw3_err.txt
EC=$?
if [ $EC -ne 0 ]; then
    pass "sorting_floor >= num_floors: gecersiz olarak reddedildi"
else
    fail "sorting_floor >= num_floors: program basarili olmamali"
fi
rm -f /tmp/hw3_bad_floor.txt

# ============================================================
# ============================================================
# TEST GRUBU 3: Temel Fonksiyonellik
# ============================================================
# ============================================================
section "3. Temel Fonksiyonellik (input.txt)"

OUT_BASIC="/tmp/hw3_basic_out.txt"
STDOUT_BASIC="/tmp/hw3_basic_stdout.txt"
STDERR_BASIC="/tmp/hw3_basic_stderr.txt"
rm -f "$OUT_BASIC"

info "Calistiriliyor: $BINARY -f 3 -w 2 -l 2 -s 2 -c 5 -d 8 -r 5 -i $INPUT -o $OUT_BASIC"
timeout "$RUN_TIMEOUT" "$BINARY" \
    -f 3 -w 2 -l 2 -s 2 -c 5 -d 8 -r 5 \
    -i "$INPUT" -o "$OUT_BASIC" \
    > "$STDOUT_BASIC" 2> "$STDERR_BASIC"
BASIC_EC=$?

# 3.1 Cikis kodu
if [ $BASIC_EC -eq 0 ]; then
    pass "Program basariyla tamamlandi (cikis kodu 0)"
elif [ $BASIC_EC -eq 124 ]; then
    fail "Program ZAMAN ASIMINA UGRADI (>${RUN_TIMEOUT}s) - olasi deadlock!"
    info "Geri kalan testler ciktisiz devam edecek"
else
    fail "Program basarisiz cikti (kod: $BASIC_EC)"
    info "Stderr: $(cat $STDERR_BASIC | head -5)"
fi

# 3.2 Output dosyasi olusturuldu mu
if [ -f "$OUT_BASIC" ]; then
    pass "Output dosyasi olusturuldu: $OUT_BASIC"
else
    fail "Output dosyasi OLUSTURULMADI"
fi

# ============================================================
# ============================================================
# TEST GRUBU 4: Output Dosyasi Icerigi
# ============================================================
# ============================================================
section "4. Output Dosyasi Formati ve Dogrulugu"

if [ ! -f "$OUT_BASIC" ]; then
    skip "Output dosyasi yok, grup 4 atlaniyor"
else
    # Girdi kelime sayisi
    INPUT_WC=$(grep -c "[^[:space:]]" "$INPUT" 2>/dev/null || echo "0")
    # Cikti satir sayisi (bos satir olmadan)
    OUTPUT_LC=$(grep -c "[^[:space:]]" "$OUT_BASIC" 2>/dev/null || echo "0")

    # 4.1 Bos satir yok
    BLANK=$(grep "^[[:space:]]*$" "$OUT_BASIC" 2>/dev/null | wc -l)
    if [ "$BLANK" -eq 0 ]; then
        pass "Output dosyasinda bos satir yok"
    else
        fail "Output dosyasinda $BLANK bos satir var"
    fi

    # 4.2 Dogru satir sayisi
    if [ "$OUTPUT_LC" -eq "$INPUT_WC" ]; then
        pass "Output satir sayisi dogru ($OUTPUT_LC = $INPUT_WC kelime)"
    else
        fail "Output $OUTPUT_LC satir, beklenen $INPUT_WC"
    fi

    # 4.3 Her satir tam olarak 3 alan icermeli (word_id word sorting_floor)
    MALFORMED=$(awk 'NF != 3 { c++ } END { print c+0 }' "$OUT_BASIC")
    if [ "$MALFORMED" -eq 0 ]; then
        pass "Tum satirlar tam olarak 3 alan icerir (word_id kelime sorting_floor)"
    else
        fail "$MALFORMED satir yanlis alan sayisina sahip"
    fi

    # 4.4 Girdideki tum word_id'ler outputta mevcut mu
    ALL_IDS_OK=1
    while IFS=" " read -r wid word sf; do
        if ! grep -q "^${wid} " "$OUT_BASIC"; then
            fail "word_id=$wid outputta bulunamadi"
            ALL_IDS_OK=0
        fi
    done < "$INPUT"
    [ "$ALL_IDS_OK" -eq 1 ] && pass "Tum word_id'ler outputta mevcut"

    # 4.5 Outputtaki kelimeler girdiyle eslesiyormu
    WORD_OK=1
    while IFS=" " read -r wid word sf; do
        OUT_WORD=$(grep "^${wid} " "$OUT_BASIC" | awk '{print $2}')
        if [ "$OUT_WORD" != "$word" ]; then
            fail "word_id=$wid: beklenen kelime='$word', elde edilen='$OUT_WORD'"
            WORD_OK=0
        fi
    done < "$INPUT"
    [ "$WORD_OK" -eq 1 ] && pass "Outputtaki kelimeler girdiyle esliyor"

    # 4.6 Outputtaki sorting_floor'lar girdiyle esliyor
    SF_OK=1
    while IFS=" " read -r wid word sf; do
        OUT_SF=$(grep "^${wid} " "$OUT_BASIC" | awk '{print $3}')
        if [ "$OUT_SF" != "$sf" ]; then
            fail "word_id=$wid: beklenen sorting_floor=$sf, elde edilen=$OUT_SF"
            SF_OK=0
        fi
    done < "$INPUT"
    [ "$SF_OK" -eq 1 ] && pass "Sorting floor'lar dogru (orijinal girdi degerlerine esit)"

    # 4.7 Output siralama: once sorting_floor, sonra word_id
    # Beklenen sira: sort -k3,3n -k1,1n uygulayarak
    EXPECTED_IDS=$(sort -k3,3n -k1,1n "$INPUT" | awk '{print $1}' | tr '\n' ' ')
    ACTUAL_IDS=$(awk '{print $1}' "$OUT_BASIC" | tr '\n' ' ')
    if [ "$EXPECTED_IDS" = "$ACTUAL_IDS" ]; then
        pass "Output dogru sirada: once sorting_floor, sonra word_id"
    else
        fail "Output siralama hatali"
        info "Beklenen: $EXPECTED_IDS"
        info "Elde:     $ACTUAL_IDS"
    fi

    # 4.8 input.txt icin kesin beklenen output kontrolu
    # 101 apple 2 | 102 process 0 | 103 system 1 | 104 kernel 2 | 105 thread 0
    # Sira: floor0:(102,105) floor1:(103) floor2:(101,104)
    EXP1="102 process 0"
    EXP2="105 thread 0"
    EXP3="103 system 1"
    EXP4="101 apple 2"
    EXP5="104 kernel 2"
    ACT1=$(sed -n '1p' "$OUT_BASIC")
    ACT2=$(sed -n '2p' "$OUT_BASIC")
    ACT3=$(sed -n '3p' "$OUT_BASIC")
    ACT4=$(sed -n '4p' "$OUT_BASIC")
    ACT5=$(sed -n '5p' "$OUT_BASIC")
    if [ "$ACT1" = "$EXP1" ] && [ "$ACT2" = "$EXP2" ] && \
       [ "$ACT3" = "$EXP3" ] && [ "$ACT4" = "$EXP4" ] && \
       [ "$ACT5" = "$EXP5" ]; then
        pass "Output iceriginin beklenen degerlerle tamamen eslesmesi"
    else
        fail "Output icerik hatasi"
        info "Beklenen:"
        printf "    %s\n    %s\n    %s\n    %s\n    %s\n" \
            "$EXP1" "$EXP2" "$EXP3" "$EXP4" "$EXP5"
        info "Elde edilen:"
        sed 's/^/    /' "$OUT_BASIC"
    fi

    # 4.9 Arrival floor output'u etkilememeli (output sadece sorting_floor icermeli)
    # input.txt'deki tum sorting_floor'lar: 0,0,1,2,2 -> output floor degerleri
    UNEXPECTED_FLOORS=$(awk 'NR>0 {print $3}' "$OUT_BASIC" | grep -v "^[012]$" | wc -l)
    if [ "$UNEXPECTED_FLOORS" -eq 0 ]; then
        pass "Output floor degerleri yalnizca beklenen sorting_floor'lar (0,1,2)"
    else
        fail "Output'ta beklenmeyen floor degerleri var"
    fi
fi

# ============================================================
# ============================================================
# TEST GRUBU 5: Terminal Ozet Ciktisi
# ============================================================
# ============================================================
section "5. Terminal Ozet Ciktisi (System Summary)"

if [ ! -f "$STDOUT_BASIC" ]; then
    skip "stdout dosyasi yok, grup 5 atlaniyor"
else
    # 5.1 "Total words"
    grep -qi "total words" "$STDOUT_BASIC" \
        && pass "Summary: 'Total words' yazdirildi" \
        || fail "Summary: 'Total words' BULUNAMADI"

    # 5.2 "Completed words"
    grep -qi "completed words" "$STDOUT_BASIC" \
        && pass "Summary: 'Completed words' yazdirildi" \
        || fail "Summary: 'Completed words' BULUNAMADI"

    # 5.3 "Retries"
    grep -qi "retries" "$STDOUT_BASIC" \
        && pass "Summary: 'Retries' yazdirildi" \
        || fail "Summary: 'Retries' BULUNAMADI"

    # 5.4 "Characters transported"
    grep -qi "characters transported" "$STDOUT_BASIC" \
        && pass "Summary: 'Characters transported' yazdirildi" \
        || fail "Summary: 'Characters transported' BULUNAMADI"

    # 5.5 "Delivery elevator operations"
    grep -qi "delivery elevator" "$STDOUT_BASIC" \
        && pass "Summary: 'Delivery elevator operations' yazdirildi" \
        || fail "Summary: 'Delivery elevator operations' BULUNAMADI"

    # 5.6 "Reposition elevator operations"
    grep -qi "reposition elevator" "$STDOUT_BASIC" \
        && pass "Summary: 'Reposition elevator operations' yazdirildi" \
        || fail "Summary: 'Reposition elevator operations' BULUNAMADI"

    # 5.7 Total words degeri girdi dosyasiyla eslesiyor
    TOTAL_REP=$(grep -i "total words" "$STDOUT_BASIC" | grep -o "[0-9]*" | head -1)
    INPUT_WC2=$(grep -c "[^[:space:]]" "$INPUT" 2>/dev/null || echo "0")
    if [ -n "$TOTAL_REP" ] && [ "$TOTAL_REP" = "$INPUT_WC2" ]; then
        pass "Total words ($TOTAL_REP) girdi sayisiyla esliyor ($INPUT_WC2)"
    else
        fail "Total words uyusmazligi: rapor=$TOTAL_REP, girdi=$INPUT_WC2"
    fi

    # 5.8 Completed words == Total words (tum kelimeler tamamlandi)
    COMP_REP=$(grep -i "completed words" "$STDOUT_BASIC" | grep -o "[0-9]*" | head -1)
    if [ -n "$COMP_REP" ] && [ -n "$TOTAL_REP" ] && [ "$COMP_REP" = "$TOTAL_REP" ]; then
        pass "Completed words ($COMP_REP) = Total words -> tum kelimeler tamamlandi"
    else
        fail "Completed words ($COMP_REP) != Total words ($TOTAL_REP)"
    fi

    # 5.9 Characters transported == toplam karakter sayisi
    # apple(5)+process(7)+system(6)+kernel(6)+thread(6) = 30
    EXPECTED_CHARS=0
    while IFS=" " read -r wid word sf; do
        EXPECTED_CHARS=$((EXPECTED_CHARS + ${#word}))
    done < "$INPUT"
    CHARS_REP=$(grep -i "characters transported" "$STDOUT_BASIC" | grep -o "[0-9]*" | head -1)
    if [ -n "$CHARS_REP" ] && [ "$CHARS_REP" -eq "$EXPECTED_CHARS" ]; then
        pass "Characters transported ($CHARS_REP) beklenen toplam ile esliyor ($EXPECTED_CHARS)"
    else
        fail "Characters transported: rapor=$CHARS_REP, beklenen=$EXPECTED_CHARS"
    fi

    # 5.10 "Program terminated successfully"
    grep -qi "terminated successfully" "$STDOUT_BASIC" \
        && pass "'Program terminated successfully.' mesaji yazdirildi" \
        || fail "'Program terminated successfully.' mesaji BULUNAMADI"

    # 5.11 Baslangic mesajlari
    grep -q "Program is starting" "$STDOUT_BASIC" \
        && pass "'Program is starting...' mesaji mevcut" \
        || fail "'Program is starting...' mesaji bulunamadi"

    grep -q "Shared memory is initialized" "$STDOUT_BASIC" \
        && pass "'Shared memory is initialized...' mesaji mevcut" \
        || fail "'Shared memory is initialized...' mesaji bulunamadi"

    grep -q "Processes are being created" "$STDOUT_BASIC" \
        && pass "'Processes are being created...' mesaji mevcut" \
        || fail "'Processes are being created...' mesaji bulunamadi"
fi

# ============================================================
# ============================================================
# TEST GRUBU 6: Paylasmali Bellek Temizligi
# ============================================================
# ============================================================
section "6. Paylasmali Bellek (Shared Memory) Temizligi"

if ! command -v ipcs &>/dev/null; then
    skip "ipcs komutu mevcut degil, shm temizlik testi atlaniyor"
else
    SHM_BEFORE=$(ipcs -m 2>/dev/null | awk 'NR>3 && $1 ~ /^0x/{c++} END{print c+0}')
    timeout "$RUN_TIMEOUT" "$BINARY" \
        -f 3 -w 2 -l 2 -s 2 -c 5 -d 8 -r 5 \
        -i "$INPUT" -o /tmp/hw3_shm_out.txt \
        > /dev/null 2>&1
    sleep 1
    SHM_AFTER=$(ipcs -m 2>/dev/null | awk 'NR>3 && $1 ~ /^0x/{c++} END{print c+0}')
    if [ "$SHM_AFTER" -le "$SHM_BEFORE" ]; then
        pass "Paylasmali bellek segmenti sizintisi yok (once=$SHM_BEFORE, sonra=$SHM_AFTER)"
    else
        fail "Paylasmali bellek sizintisi: once=$SHM_BEFORE, sonra=$SHM_AFTER"
    fi
fi

# ============================================================
# ============================================================
# TEST GRUBU 7: Kenar Durumlar ve Ozel Konfigurasyonlar
# ============================================================
# ============================================================
section "7. Kenar Durumlar"

# 7.1 Minimum konfigurasyon (her parametreden 1 tane)
OUT_MIN="/tmp/hw3_min_out.txt"
info "Minimum konfigurasyon: -f 3 -w 1 -l 1 -s 1 -c 3 -d 1 -r 1"
timeout "$RUN_TIMEOUT" "$BINARY" \
    -f 3 -w 1 -l 1 -s 1 -c 3 -d 1 -r 1 \
    -i "$INPUT" -o "$OUT_MIN" > /dev/null 2>&1
EC=$?
if [ $EC -eq 0 ] && [ -f "$OUT_MIN" ]; then
    LC=$(grep -c "[^[:space:]]" "$OUT_MIN" 2>/dev/null || echo "0")
    [ "$LC" -eq 5 ] \
        && pass "Minimum konfigurasyon: 5 kelime dogru tamamlandi" \
        || fail "Minimum konfigurasyon: $LC satir (beklenen 5)"
elif [ $EC -eq 124 ]; then
    fail "Minimum konfigurasyon: ZAMAN ASIMI (olasi deadlock)"
else
    fail "Minimum konfigurasyon: cikis kodu $EC"
fi

# 7.2 Tek kat (tek floor, hepsi floor 0'da)
cat > /tmp/hw3_sf_single.txt << 'EOF'
201 cat 0
202 dog 0
203 rat 0
EOF
OUT_SF="/tmp/hw3_sf_out.txt"
info "Tek kat testi: -f 1"
timeout "$RUN_TIMEOUT" "$BINARY" \
    -f 1 -w 2 -l 2 -s 2 -c 5 -d 5 -r 3 \
    -i /tmp/hw3_sf_single.txt -o "$OUT_SF" > /dev/null 2>&1
EC=$?
if [ $EC -eq 0 ] && [ -f "$OUT_SF" ]; then
    LC=$(grep -c "[^[:space:]]" "$OUT_SF" 2>/dev/null || echo "0")
    [ "$LC" -eq 3 ] \
        && pass "Tek kat: 3 kelime dogru tamamlandi" \
        || fail "Tek kat: $LC satir (beklenen 3)"
elif [ $EC -eq 124 ]; then
    fail "Tek kat: ZAMAN ASIMI"
else
    fail "Tek kat: cikis kodu $EC"
fi

# 7.3 Tek kelime
cat > /tmp/hw3_single_word.txt << 'EOF'
301 hello 0
EOF
OUT_SW="/tmp/hw3_sw_out.txt"
info "Tek kelime testi"
timeout "$RUN_TIMEOUT" "$BINARY" \
    -f 2 -w 1 -l 1 -s 1 -c 2 -d 3 -r 2 \
    -i /tmp/hw3_single_word.txt -o "$OUT_SW" > /dev/null 2>&1
EC=$?
if [ $EC -eq 0 ] && [ -f "$OUT_SW" ]; then
    CONTENT=$(cat "$OUT_SW")
    [ "$CONTENT" = "301 hello 0" ] \
        && pass "Tek kelime: output dogru ('301 hello 0')" \
        || fail "Tek kelime: yanlis output ('$CONTENT')"
elif [ $EC -eq 124 ]; then
    fail "Tek kelime: ZAMAN ASIMI"
else
    fail "Tek kelime: cikis kodu $EC"
fi

# 7.4 Varris ve siralama kati ayni (aynı katta kalacak karakter)
cat > /tmp/hw3_same_fl.txt << 'EOF'
401 go 0
EOF
OUT_SAME="/tmp/hw3_same_out.txt"
info "Varris = Siralama kati testi (floor 0 -> floor 0)"
timeout "$RUN_TIMEOUT" "$BINARY" \
    -f 2 -w 1 -l 1 -s 1 -c 3 -d 3 -r 2 \
    -i /tmp/hw3_same_fl.txt -o "$OUT_SAME" > /dev/null 2>&1
EC=$?
if [ $EC -eq 0 ] && [ -f "$OUT_SAME" ]; then
    CONTENT=$(cat "$OUT_SAME")
    [ "$CONTENT" = "401 go 0" ] \
        && pass "Ayni kat: output dogru ('401 go 0')" \
        || fail "Ayni kat: yanlis output ('$CONTENT')"
elif [ $EC -eq 124 ]; then
    fail "Ayni kat: ZAMAN ASIMI"
else
    fail "Ayni kat: cikis kodu $EC"
fi

# 7.5 Tam gerekli kat sayisi (input.txt'teki max sorting_floor + 1)
# Not: Fazla kat (orn. -f 10) letter-carrier process patlamasina yol acar:
# bos katlardaki carrier'lar hic is bulamaz -> reposition dongusune girer ->
# parent her check'te o kata yeni carrier forkluyarak process sayisi patlar.
# Bu nedenle -f degerini her zaman (max_sorting_floor + 1) olarak vermek gerekir.
MAX_SF_INPUT=$(awk '{print $3}' "$INPUT" | sort -n | tail -1)
EXACT_FLOORS=$((MAX_SF_INPUT + 1))
OUT_HF="/tmp/hw3_hf_out.txt"
info "Minimum gerekli kat testi: -f $EXACT_FLOORS (max sorting_floor=$MAX_SF_INPUT)"
timeout "$RUN_TIMEOUT" "$BINARY" \
    -f "$EXACT_FLOORS" -w 2 -l 3 -s 2 -c 5 -d 8 -r 5 \
    -i "$INPUT" -o "$OUT_HF" > /dev/null 2>&1
EC=$?
if [ $EC -eq 0 ] && [ -f "$OUT_HF" ]; then
    LC=$(grep -c "[^[:space:]]" "$OUT_HF" 2>/dev/null || echo "0")
    [ "$LC" -eq 5 ] \
        && pass "Minimum gerekli kat (-f $EXACT_FLOORS): tum kelimeler tamamlandi" \
        || fail "Minimum gerekli kat: $LC satir (beklenen 5)"
elif [ $EC -eq 124 ]; then
    fail "Minimum gerekli kat: ZAMAN ASIMI"
else
    fail "Minimum gerekli kat: cikis kodu $EC"
fi

# 7.6 Buyuk kapasiteler (hicbir kisitlama yok)
OUT_LC="/tmp/hw3_lc_out.txt"
info "Buyuk kapasite testi: -c 100 -d 50 -r 20"
timeout "$RUN_TIMEOUT" "$BINARY" \
    -f 3 -w 2 -l 2 -s 2 -c 100 -d 50 -r 20 \
    -i "$INPUT" -o "$OUT_LC" > /dev/null 2>&1
EC=$?
if [ $EC -eq 0 ] && [ -f "$OUT_LC" ]; then
    pass "Buyuk kapasiteler: program basaryla tamamlandi"
elif [ $EC -eq 124 ]; then
    fail "Buyuk kapasiteler: ZAMAN ASIMI"
else
    fail "Buyuk kapasiteler: cikis kodu $EC"
fi

# 7.7 Dar kapasite (c=1, cok sayida retry beklenir)
OUT_TC="/tmp/hw3_tc_out.txt"
STDOUT_TC="/tmp/hw3_tc_stdout.txt"
info "Dar kapasite testi: -c 1 (yuksek retry beklenir)"
timeout "$RUN_TIMEOUT" "$BINARY" \
    -f 3 -w 2 -l 2 -s 2 -c 1 -d 2 -r 2 \
    -i "$INPUT" -o "$OUT_TC" > "$STDOUT_TC" 2>&1
EC=$?
if [ $EC -eq 0 ] && [ -f "$OUT_TC" ]; then
    LC=$(grep -c "[^[:space:]]" "$OUT_TC" 2>/dev/null || echo "0")
    if [ "$LC" -eq 5 ]; then
        pass "Dar kapasite (c=1): tum kelimeler tamamlandi"
        # Retry sayisi pozitif olmali
        RETRY_CNT=$(grep -i "retries" "$STDOUT_TC" | grep -o "[0-9]*" | head -1)
        if [ -n "$RETRY_CNT" ] && [ "$RETRY_CNT" -gt 0 ]; then
            pass "Dar kapasite: retry sayisi pozitif ($RETRY_CNT)"
        else
            info "Retry sayisi: $RETRY_CNT (c=1 ile retry beklenirdi)"
        fi
    else
        fail "Dar kapasite: $LC satir (beklenen 5)"
    fi
elif [ $EC -eq 124 ]; then
    fail "Dar kapasite (c=1): ZAMAN ASIMI - olasi deadlock!"
else
    fail "Dar kapasite: cikis kodu $EC"
fi

# 7.8 Coklu kelime, ayni sorting floor
cat > /tmp/hw3_same_sf.txt << 'EOF'
601 abc 1
602 def 1
603 ghi 1
EOF
OUT_SSF="/tmp/hw3_ssf_out.txt"
info "Tum kelimeler ayni sorting_floor testi"
timeout "$RUN_TIMEOUT" "$BINARY" \
    -f 3 -w 2 -l 2 -s 2 -c 5 -d 5 -r 3 \
    -i /tmp/hw3_same_sf.txt -o "$OUT_SSF" > /dev/null 2>&1
EC=$?
if [ $EC -eq 0 ] && [ -f "$OUT_SSF" ]; then
    # word_id sirasi: 601, 602, 603 (hepsi floor 1)
    IDS=$(awk '{print $1}' "$OUT_SSF" | tr '\n' ' ')
    [ "$IDS" = "601 602 603 " ] \
        && pass "Ayni sorting_floor: word_id sirasina gore dogru siralama" \
        || fail "Ayni sorting_floor: yanlis siralama (elde: $IDS)"
elif [ $EC -eq 124 ]; then
    fail "Ayni sorting_floor: ZAMAN ASIMI"
else
    fail "Ayni sorting_floor: cikis kodu $EC"
fi

# ============================================================
# ============================================================
# TEST GRUBU 8: Eszamanlilik ve Deadlock Yok
# ============================================================
# ============================================================
section "8. Eszamanlilik ve Deadlock Yok"

# 8.1 Yuksek eszamanlilik: cok sayida process
OUT_HIGH="/tmp/hw3_high_out.txt"
info "Yuksek eszamanlilik: -w 3 -l 4 -s 3"
timeout "$RUN_TIMEOUT" "$BINARY" \
    -f 3 -w 3 -l 4 -s 3 -c 5 -d 10 -r 5 \
    -i "$INPUT" -o "$OUT_HIGH" > /dev/null 2>&1
EC=$?
if [ $EC -eq 0 ] && [ -f "$OUT_HIGH" ]; then
    LC=$(grep -c "[^[:space:]]" "$OUT_HIGH" 2>/dev/null || echo "0")
    [ "$LC" -eq 5 ] \
        && pass "Yuksek esz. (w=3,l=4,s=3): tum 5 kelime tamamlandi" \
        || fail "Yuksek esz.: $LC satir (beklenen 5)"
elif [ $EC -eq 124 ]; then
    fail "Yuksek esz.: ZAMAN ASIMI"
else
    fail "Yuksek esz.: cikis kodu $EC"
fi

# 8.2 Belirleyicilik: ayni parametrelerle iki calistirma ayni output uretmeli
OUT_R1="/tmp/hw3_run1.txt"
OUT_R2="/tmp/hw3_run2.txt"
timeout "$RUN_TIMEOUT" "$BINARY" \
    -f 3 -w 2 -l 2 -s 2 -c 5 -d 8 -r 5 \
    -i "$INPUT" -o "$OUT_R1" > /dev/null 2>&1
timeout "$RUN_TIMEOUT" "$BINARY" \
    -f 3 -w 2 -l 2 -s 2 -c 5 -d 8 -r 5 \
    -i "$INPUT" -o "$OUT_R2" > /dev/null 2>&1
if [ -f "$OUT_R1" ] && [ -f "$OUT_R2" ]; then
    if diff -q "$OUT_R1" "$OUT_R2" > /dev/null 2>&1; then
        pass "Belirleyicilik: iki calistirma ayni output uretir"
    else
        fail "Belirleyicilik: farkli outputlar uretildi (non-deterministic)"
        info "1. calistirma: $(cat $OUT_R1)"
        info "2. calistirma: $(cat $OUT_R2)"
    fi
else
    fail "Belirleyicilik: bir veya iki calistirma output uretmedi"
fi

# 8.3 Zombie process kontrolu
timeout "$RUN_TIMEOUT" "$BINARY" \
    -f 3 -w 2 -l 2 -s 2 -c 5 -d 8 -r 5 \
    -i "$INPUT" -o /tmp/hw3_zombie_out.txt > /dev/null 2>&1
sleep 1
ZOMBIE=$(ps aux 2>/dev/null | awk '$8 == "Z" {c++} END {print c+0}')
if [ "$ZOMBIE" -eq 0 ]; then
    pass "Zombie process yok"
else
    fail "$ZOMBIE zombie process bulundu"
fi

# 8.4 Process sayisi tamamlanma sonrasi normal
# (Parent ve cocuklar dogru sekilde sonlanmali)
PID_BEFORE=$(pgrep -c hw3 2>/dev/null || echo "0")
timeout "$RUN_TIMEOUT" "$BINARY" \
    -f 3 -w 2 -l 2 -s 2 -c 5 -d 8 -r 5 \
    -i "$INPUT" -o /tmp/hw3_proc_out.txt > /dev/null 2>&1
sleep 1
PID_AFTER=$(pgrep -c hw3 2>/dev/null || echo "0")
if [ "$PID_AFTER" -le "$PID_BEFORE" ]; then
    pass "Tamamlanma sonrasi hw3 process'i kalmadi"
else
    fail "Tamamlanma sonrasi $PID_AFTER hw3 process hala calisiyor"
fi

# ============================================================
# ============================================================
# TEST GRUBU 9: Buyuk Input (input_big.txt)
# ============================================================
# ============================================================
section "9. Buyuk Input Dosyasi (input_big.txt)"

if [ ! -f "input_big.txt" ]; then
    skip "input_big.txt bulunamadi, grup 9 atlaniyor"
else
    BIG_WC=$(grep -c "[^[:space:]]" input_big.txt 2>/dev/null || echo "0")
    MAX_SF=$(awk '{print $3}' input_big.txt | sort -n | tail -1)
    NEEDED_FLOORS=$((MAX_SF + 1))
    info "input_big.txt: $BIG_WC kelime, max sorting_floor=$MAX_SF, gereken floor sayisi=$NEEDED_FLOORS"

    OUT_BIG="/tmp/hw3_big_out.txt"
    STDOUT_BIG="/tmp/hw3_big_stdout.txt"

    info "Buyuk input calistiriliyor: -f $NEEDED_FLOORS -w 2 -l 3 -s 2 -c 10 -d 10 -r 5"
    timeout "$BIG_TIMEOUT" "$BINARY" \
        -f "$NEEDED_FLOORS" -w 2 -l 3 -s 2 -c 10 -d 10 -r 5 \
        -i input_big.txt -o "$OUT_BIG" \
        > "$STDOUT_BIG" 2>&1
    EC=$?

    if [ $EC -eq 0 ] && [ -f "$OUT_BIG" ]; then
        BIG_OUT_LC=$(grep -c "[^[:space:]]" "$OUT_BIG" 2>/dev/null || echo "0")
        if [ "$BIG_OUT_LC" -eq "$BIG_WC" ]; then
            pass "Buyuk input: tum $BIG_WC kelime islendi"
        else
            fail "Buyuk input: $BIG_OUT_LC satir (beklenen $BIG_WC)"
        fi

        # Siralama kontrolu
        EXP_BIG=$(sort -k3,3n -k1,1n input_big.txt | awk '{print $1}' | tr '\n' ' ')
        ACT_BIG=$(awk '{print $1}' "$OUT_BIG" | tr '\n' ' ')
        if [ "$EXP_BIG" = "$ACT_BIG" ]; then
            pass "Buyuk input: siralama dogru (sorting_floor, word_id)"
        else
            fail "Buyuk input: siralama hatali"
        fi

        # Karakter sayisi kontrolu
        BIG_CHARS=0
        while IFS=" " read -r wid word sf; do
            BIG_CHARS=$((BIG_CHARS + ${#word}))
        done < input_big.txt
        BIG_CHARS_REP=$(grep -i "characters transported" "$STDOUT_BIG" | grep -o "[0-9]*" | head -1)
        if [ -n "$BIG_CHARS_REP" ] && [ "$BIG_CHARS_REP" -eq "$BIG_CHARS" ]; then
            pass "Buyuk input: characters transported ($BIG_CHARS_REP) dogru"
        else
            fail "Buyuk input: characters transported rapor=$BIG_CHARS_REP beklenen=$BIG_CHARS"
        fi

    elif [ $EC -eq 124 ]; then
        fail "Buyuk input: ZAMAN ASIMI (>${BIG_TIMEOUT}s) - olasi deadlock!"
    else
        fail "Buyuk input: cikis kodu $EC"
        head -5 "$STDOUT_BIG" | sed 's/^/    /'
    fi
fi

# ============================================================
# ============================================================
# TEST GRUBU 10: Coklu Calistirma - Tutarlilik
# ============================================================
# ============================================================
section "10. Coklu Calistirma Tutarliligi"

info "5 ardisik calistirma - hepsi ayni output uretmeli"
CONSISTENT=1
PREV_OUT=""
for i in 1 2 3 4 5; do
    CURR="/tmp/hw3_run_${i}.txt"
    timeout "$RUN_TIMEOUT" "$BINARY" \
        -f 3 -w 2 -l 2 -s 2 -c 5 -d 8 -r 5 \
        -i "$INPUT" -o "$CURR" > /dev/null 2>&1
    if [ $? -ne 0 ] || [ ! -f "$CURR" ]; then
        fail "Calistirma #$i basarisiz"
        CONSISTENT=0
        break
    fi
    if [ -n "$PREV_OUT" ]; then
        if ! diff -q "$PREV_OUT" "$CURR" > /dev/null 2>&1; then
            fail "Calistirma #$i oncekiyle farkli output uretti"
            CONSISTENT=0
        fi
    fi
    PREV_OUT="$CURR"
done
[ "$CONSISTENT" -eq 1 ] && pass "5 calistirma tutarli (identical output)"

# ============================================================
# Son Temizlik
# ============================================================
cleanup_tmp

# ============================================================
# ============================================================
# SONUC RAPORU
# ============================================================
# ============================================================
section "Test Sonuclari"

echo ""
echo -e "  Toplam Test : $TOTAL"
echo -e "  ${GREEN}Gecti       : $PASS${NC}"
echo -e "  ${RED}Kaldi       : $FAIL${NC}"
echo -e "  ${CYAN}Atlandi     : $SKIP${NC}"
echo ""

PERCENT=0
if [ "$TOTAL" -gt 0 ]; then
    PERCENT=$(( (PASS * 100) / TOTAL ))
fi
echo -e "  Basari Orani: %$PERCENT"
echo ""

if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}Tum testler gecti!${NC}"
    exit 0
else
    echo -e "${RED}$FAIL test basarisiz.${NC}"
    exit 1
fi
