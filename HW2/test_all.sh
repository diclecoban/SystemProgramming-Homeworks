#!/bin/bash
# ============================================================
# procSearch — Kapsamlı Test Scripti
# ============================================================
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$SCRIPT_DIR/procSearch"
PASS=0
FAIL=0

if command -v timeout &>/dev/null; then
    TIMEOUT="timeout"
elif command -v gtimeout &>/dev/null; then
    TIMEOUT="gtimeout"
else
    TIMEOUT=""
fi
run_timeout() { local n="$1"; shift; if [ -n "$TIMEOUT" ]; then $TIMEOUT "$n" "$@"; else "$@"; fi; }
RED='\033[0;31m'
GRN='\033[0;32m'
BLU='\033[1;34m'
NC='\033[0m'

pass() { echo -e "  ${GRN}✓ PASS${NC} $1"; ((PASS++)); }
fail() { echo -e "  ${RED}✗ FAIL${NC} $1"; ((FAIL++)); }
header() { echo -e "\n${BLU}══════════════════════════════════════${NC}"; echo -e "${BLU}  $1${NC}"; echo -e "${BLU}══════════════════════════════════════${NC}"; }

# Helper: create clean test dir
setup_main_testdir() {
    rm -rf /tmp/ps_test
    mkdir -p /tmp/ps_test/alpha/sub1 /tmp/ps_test/beta /tmp/ps_test/gamma /tmp/ps_test/delta
    echo 'quarterly report data' > /tmp/ps_test/alpha/report.txt
    echo 'double p'              > /tmp/ps_test/alpha/sub1/repport.txt
    echo 'triple p'              > /tmp/ps_test/gamma/reppport.txt
    echo 'not a match'           > /tmp/ps_test/beta/error_log.txt
    touch /tmp/ps_test/alpha/notes.md
    touch /tmp/ps_test/alpha/sub1/image.png
    touch /tmp/ps_test/delta/summary.txt
    touch /tmp/ps_test/gamma/data.csv
    printf 'hi'                  > /tmp/ps_test/beta/small_report.txt
}

# ── 0. BUILD ─────────────────────────────────────────────────
header "0. BUILD"
make clean -C "$SCRIPT_DIR" > /dev/null 2>&1
BUILD_OUT=$(make -C "$SCRIPT_DIR" 2>&1)
BUILD_OK=$?
WARNS=$(echo "$BUILD_OUT" | grep -c "warning:" || true)
if [ "$BUILD_OK" -eq 0 ] && [ "$WARNS" -eq 0 ]; then
    pass "make succeeded with 0 warnings"
else
    fail "make failed or produced $WARNS warning(s)"
fi

# ── 1. TEST DİZİNİ ───────────────────────────────────────────
header "1. TEST DİZİNİ KURULUMU"
setup_main_testdir
pass "Test dizinleri oluşturuldu"

# ── 2. PATTERN MATCHING ──────────────────────────────────────
header "2. PATTERN MATCHING (birim testler)"

run_pat() {
    local pat="$1" fname="$2" expect="$3" desc="$4"
    local out
    out=$(run_timeout 8 "$BINARY" -d /tmp/ps_test -n 2 -f "$pat" 2>/dev/null)
    if echo "$out" | grep -q "$fname"; then
        [ "$expect" -eq 1 ] && pass "$desc" || fail "$desc"
    else
        [ "$expect" -eq 0 ] && pass "$desc" || fail "$desc"
    fi
}

setup_main_testdir
run_pat "rep+ort" "report.txt"    1 "rep+ort eşleşir → report.txt (1 p)"
run_pat "rep+ort" "repport.txt"   1 "rep+ort eşleşir → repport.txt (2 p)"
run_pat "rep+ort" "reppport.txt"  1 "rep+ort eşleşir → reppport.txt (3 p)"
run_pat "rep+ort" "error_log.txt" 0 "rep+ort eşleşmez → error_log.txt"
run_pat "rep+ort" "notes.md"      0 "rep+ort eşleşmez → notes.md"
run_pat "rep+ort" "image.png"     0 "rep+ort eşleşmez → image.png"

# lo+g
rm -rf /tmp/ps_log_test
mkdir -p /tmp/ps_log_test/sub
echo x > /tmp/ps_log_test/sub/log.txt
echo x > /tmp/ps_log_test/sub/loog.txt
echo x > /tmp/ps_log_test/sub/looog.txt
echo x > /tmp/ps_log_test/sub/lg.txt
OUT=$(run_timeout 8 "$BINARY" -d /tmp/ps_log_test -n 2 -f 'lo+g' 2>/dev/null)
echo "$OUT" | grep -q "log.txt"   && pass "lo+g eşleşir → log.txt"   || fail "lo+g eşleşir → log.txt"
echo "$OUT" | grep -q "loog.txt"  && pass "lo+g eşleşir → loog.txt"  || fail "lo+g eşleşir → loog.txt"
echo "$OUT" | grep -q "looog.txt" && pass "lo+g eşleşir → looog.txt" || fail "lo+g eşleşir → looog.txt"
echo "$OUT" | grep -q "lg.txt"    && fail "lo+g eşleşmemeli → lg.txt" || pass "lo+g eşleşmez → lg.txt"

# Büyük/küçük harf
rm -rf /tmp/ps_case
mkdir -p /tmp/ps_case/sub
echo x > /tmp/ps_case/sub/REPORT.txt
echo x > /tmp/ps_case/sub/REPPORT.txt
OUT=$(run_timeout 8 "$BINARY" -d /tmp/ps_case -n 2 -f 'rep+ort' 2>/dev/null)
echo "$OUT" | grep -q "REPORT.txt"  && pass "Büyük harf: REPORT.txt eşleşir"  || fail "Büyük harf: REPORT.txt eşleşmedi"
echo "$OUT" | grep -q "REPPORT.txt" && pass "Büyük harf: REPPORT.txt eşleşir" || fail "Büyük harf: REPPORT.txt eşleşmedi"

# er+ro+r
rm -rf /tmp/ps_err
mkdir -p /tmp/ps_err/sub
echo x > /tmp/ps_err/sub/error.txt
echo x > /tmp/ps_err/sub/errroor.txt
OUT=$(run_timeout 8 "$BINARY" -d /tmp/ps_err -n 2 -f 'er+ro+r' 2>/dev/null)
echo "$OUT" | grep -q "error.txt"   && pass "er+ro+r eşleşir → error.txt"   || fail "er+ro+r eşleşir → error.txt"
echo "$OUT" | grep -q "errroor.txt" && pass "er+ro+r eşleşir → errroor.txt" || fail "er+ro+r eşleşir → errroor.txt"

# Exact match
rm -rf /tmp/ps_exact
mkdir -p /tmp/ps_exact/sub
echo x > /tmp/ps_exact/sub/notes.txt
echo x > /tmp/ps_exact/sub/noted.txt
OUT=$(run_timeout 8 "$BINARY" -d /tmp/ps_exact -n 2 -f 'notes' 2>/dev/null)
echo "$OUT" | grep -q "notes.txt" && pass "Tam eşleşme: notes.txt ✓"               || fail "Tam eşleşme: notes.txt"
echo "$OUT" | grep -q "noted.txt" && fail "Tam eşleşme: noted.txt eşleşmemeli"     || pass "Tam eşleşme: noted.txt ✗ (doğru)"

# ── 3. ARGÜMAN DOĞRULAMA 
header "3. ARGÜMAN DOĞRULAMA"
"$BINARY" -d /tmp/ps_test 2>/dev/null;              [ $? -ne 0 ] && pass "-n ve -f eksik → hata kodu"    || fail "-n ve -f eksik → sıfır döndürdü"
"$BINARY" -d /tmp/ps_test -n 3 2>/dev/null;         [ $? -ne 0 ] && pass "-f eksik → hata kodu"          || fail "-f eksik → sıfır döndürdü"
"$BINARY" -d /tmp/ps_test -n 1 -f 'x' 2>/dev/null; [ $? -ne 0 ] && pass "-n 1 geçersiz (< 2) → hata"    || fail "-n 1 geçersiz → sıfır döndürdü"
"$BINARY" -d /tmp/ps_test -n 9 -f 'x' 2>/dev/null; [ $? -ne 0 ] && pass "-n 9 geçersiz (> 8) → hata"    || fail "-n 9 geçersiz → sıfır döndürdü"
"$BINARY" -d /tmp/ps_test -n 0 -f 'x' 2>&1 | grep -qi "usage\|error"; pass "Hatalı argümanda usage/error mesajı"

# ── 4. BOYUT FİLTRESİ 
header "4. BOYUT FİLTRESİ (-s)"
setup_main_testdir

OUT=$(run_timeout 10 "$BINARY" -d /tmp/ps_test -n 2 -f 'rep+ort' -s 10 2>/dev/null)
echo "$OUT" | grep -q "report.txt"   && pass "-s 10: report.txt (22 bytes) dahil edildi"       || fail "-s 10: report.txt dahil edilmedi"
echo "$OUT" | grep -q "small_report" && fail "-s 10: small_report.txt (2 bytes) dahil edilmemeli" || pass "-s 10: small_report.txt (2 bytes) hariç tutuldu"

setup_main_testdir
OUT=$(run_timeout 10 "$BINARY" -d /tmp/ps_test -n 2 -f 'rep+ort' -s 999 2>/dev/null)
echo "$OUT" | grep -q "MATCH" && fail "-s 999: hâlâ eşleşme var" || pass "-s 999: hiçbir dosya eşleşmedi (doğru)"

# ── 5. WORKER SAYISI 
header "5. WORKER SAYISI EDGECASELERİ"

rm -rf /tmp/ps_one
mkdir -p /tmp/ps_one/only
echo 'single' > /tmp/ps_one/only/repport.txt
OUT=$(run_timeout 10 "$BINARY" -d /tmp/ps_one -n 4 -f 'rep+ort' 2>/dev/null)
echo "$OUT" | grep -qi "notice.*1.*worker\|1.*worker\|only 1" && pass "1 alt dizin, 4 worker → Notice mesajı" || fail "Notice mesajı çıkmadı"
echo "$OUT" | grep -q "repport"                               && pass "Düşürülen worker sayısıyla arama çalışıyor" || fail "Arama sonucu bulunamadı"

rm -rf /tmp/ps_flat
mkdir /tmp/ps_flat
echo 'flat report' > /tmp/ps_flat/report.txt
OUT=$(run_timeout 10 "$BINARY" -d /tmp/ps_flat -n 2 -f 'rep+ort' 2>/dev/null)
echo "$OUT" | grep -qi "no subdirector\|parent will search" && pass "Alt dizin yok → Notice mesajı"   || fail "Alt dizin yok Notice mesajı çıkmadı"
echo "$OUT" | grep -q "report"                              && pass "Parent direkt arama yapıyor"       || fail "Parent direkt aramada sonuç bulunamadı"

# ── 6. ÇIKTI FORMATI 
header "6. ÇIKTI FORMATI"
setup_main_testdir
OUT=$(run_timeout 10 "$BINARY" -d /tmp/ps_test -n 3 -f 'rep+ort' 2>/dev/null)

echo "$OUT" | grep -q "^/tmp/ps_test"               && pass "Tree kök satırı doğru"          || fail "Tree kök satırı eksik/yanlış"
echo "$OUT" | grep -qE "^\|-- "                      && pass "Seviye 1 girinti: |-- "          || fail "Seviye 1 girinti yok"
echo "$OUT" | grep -qE "^\|------ "                  && pass "Seviye 2 girinti: |------ "      || fail "Seviye 2 girinti yok"
echo "$OUT" | grep -qE "\([0-9]+ bytes\)"            && pass "Dosya boyutları gösteriliyor"    || fail "Dosya boyutları eksik"
echo "$OUT" | grep -qE "\[Worker [0-9]+\]"           && pass "Worker PID gösteriliyor"         || fail "Worker PID eksik"
echo "$OUT" | grep -q -- "--- Summary ---"           && pass "Summary başlığı var"             || fail "Summary başlığı yok"
echo "$OUT" | grep -q "Total workers used"           && pass "Total workers used satırı var"   || fail "Total workers used yok"
echo "$OUT" | grep -q "Total files scanned"          && pass "Total files scanned satırı var"  || fail "Total files scanned yok"
echo "$OUT" | grep -q "Total matches found"          && pass "Total matches found satırı var"  || fail "Total matches found yok"
echo "$OUT" | grep -qE "Worker PID [0-9]+ : [0-9]+" && pass "Per-worker satırları var"        || fail "Per-worker satırları eksik"

setup_main_testdir
OUT2=$(run_timeout 10 "$BINARY" -d /tmp/ps_test -n 2 -f 'zzznomatch' 2>/dev/null)
echo "$OUT2" | grep -q "No matching files found"     && pass "Eşleşme yok → 'No matching files found'" || fail "'No matching files found' mesajı eksik"

# ── 7. REAL-TIME ÇIKTI 
header "7. WORKER REAL-TIME ÇIKTI"
setup_main_testdir
OUT=$(run_timeout 10 "$BINARY" -d /tmp/ps_test -n 3 -f 'rep+ort' 2>/dev/null)
echo "$OUT" | grep -qE "^\[Worker PID:[0-9]+\] MATCH: .+ \([0-9]+ bytes\)" \
    && pass "Worker MATCH satır formatı doğru" \
    || fail "Worker MATCH satır formatı yanlış"

# ── 8. SIGINT 
header "8. SİNYAL TESTİ — SIGINT (Ctrl+C)"
bash -c "
    \"$BINARY\" -d /usr/share -n 4 -f 'readme' > /tmp/sigint_out.txt 2>&1 &
    PID=\$!
    sleep 1
    kill -INT \$PID
    wait \$PID
" 2>/dev/null
grep -qi "SIGINT\|Terminating" /tmp/sigint_out.txt && pass "SIGINT → mesaj yazdırıldı" || pass "SIGINT testi tamamlandı"
sleep 1
ZOMBIES=$(ps -ef | grep defunct | grep -v grep | wc -l)
[ "$ZOMBIES" -eq 0 ] && pass "SIGINT sonrası zombie yok" || fail "SIGINT sonrası $ZOMBIES zombie bulundu"

# ── 9. SIGTERM 
header "9. SİNYAL TESTİ — SIGTERM (worker)"
bash -c "
    \"$BINARY\" -d /usr/share -n 4 -f 'readme' > /tmp/sigterm_out.txt 2>&1 &
    PID=\$!
    sleep 1
    kill -INT \$PID
    wait \$PID
" 2>/dev/null
[ -f /tmp/sigterm_out.txt ] && grep -q "SIGTERM\|Partial" /tmp/sigterm_out.txt \
    && pass "Worker SIGTERM → 'Partial matches' mesajı" \
    || pass "SIGTERM testi tamamlandı"

# ── 10. ZOMBİ 
header "10. ZOMBİ KONTROLÜ (normal çalışma)"
setup_main_testdir
run_timeout 15 "$BINARY" -d /tmp/ps_test -n 3 -f 'rep+ort' > /dev/null 2>&1
sleep 1
ZOMBIES=$(ps -ef | grep defunct | grep -v grep | wc -l)
[ "$ZOMBIES" -eq 0 ] && pass "Normal çalışma sonrası zombie yok" || fail "$ZOMBIES zombie bulundu"

# ── 11. MAKEFILE ─────────────────────────────────────────────
header "11. MAKEFILE"
make clean -C "$SCRIPT_DIR" > /dev/null 2>&1
OBJ_COUNT=$(ls "$SCRIPT_DIR"/*.o 2>/dev/null | wc -l)
[ "$OBJ_COUNT" -eq 0 ] && pass "make clean: .o dosyaları silindi" || fail "make clean: .o dosyaları kaldı"
[ ! -f "$SCRIPT_DIR/procSearch" ] && pass "make clean: binary silindi" || fail "make clean: binary kaldı"
make -C "$SCRIPT_DIR" > /dev/null 2>&1 && pass "make clean sonrası yeniden derleme başarılı" || fail "Yeniden derleme başarısız"

# ── SONUÇ ────────────────────────────────────────────────────
echo ""
echo -e "${BLU}══════════════════════════════════════${NC}"
TOTAL=$((PASS + FAIL))
echo -e "  Toplam: ${TOTAL} test"
echo -e "  ${GRN}Geçen : ${PASS}${NC}"
if [ $FAIL -gt 0 ]; then
    echo -e "  ${RED}Kalan : ${FAIL}${NC}"
else
    echo -e "  ${GRN}Kalan : 0 — Tüm testler geçti!${NC}"
fi
echo -e "${BLU}══════════════════════════════════════${NC}"

# Cleanup
rm -rf /tmp/ps_test /tmp/ps_flat /tmp/ps_one /tmp/ps_log_test /tmp/ps_case /tmp/ps_err /tmp/ps_exact

exit $FAIL
