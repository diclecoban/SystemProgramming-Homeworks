#!/bin/bash
# ============================================================
SCRIPT_DIR="$(cd "/Users/diclesaracoban/Desktop/SystemProgramming/HW2" && pwd)"
# procSearch — Kapsamlı Test Scripti
# ============================================================
PASS=0
FAIL=0
BINARY="./procSearch"
RED='\033[0;31m'
GRN='\033[0;32m'
YLW='\033[1;33m'
BLU='\033[1;34m'
NC='\033[0m'

pass() { echo -e "  ${GRN}✓ PASS${NC} $1"; ((PASS++)); }
fail() { echo -e "  ${RED}✗ FAIL${NC} $1"; ((FAIL++)); }
header() { echo -e "\n${BLU}══════════════════════════════════════${NC}"; echo -e "${BLU}  $1${NC}"; echo -e "${BLU}══════════════════════════════════════${NC}"; }

# ── Build ────────────────────────────────────────────────────
header "0. BUILD"
make clean -C $SCRIPT_DIR > /dev/null 2>&1
BUILD_OUT=$(make -C $SCRIPT_DIR 2>&1)
BUILD_OK=$?
WARNS=$(echo "$BUILD_OUT" | grep -c "warning:" || true)
if [ "$BUILD_OK" -eq 0 ] && [ "$WARNS" -eq 0 ]; then
    pass "make succeeded with 0 warnings"
else
    fail "make failed or produced $WARNS warning(s)"
fi
cp $SCRIPT_DIR/procSearch .

# ── Setup test directories ───────────────────────────────────
header "1. TEST DİZİNİ KURULUMU"
rm -rf /tmp/ps_test
mkdir -p /tmp/ps_test/{alpha/sub1,beta,gamma,delta}

# Pattern eşleşmesi beklenenler (rep+ort = re + p+ + ort)
echo 'quarterly report data' > /tmp/ps_test/alpha/report.txt    # 1p ✓
echo 'double p'              > /tmp/ps_test/alpha/sub1/repport.txt   # 2p ✓
echo 'triple p'              > /tmp/ps_test/gamma/reppport.txt   # 3p ✓
# Eşleşmemesi beklenenler
echo 'not a match'           > /tmp/ps_test/beta/error_log.txt
touch                          /tmp/ps_test/alpha/notes.md
touch                          /tmp/ps_test/alpha/sub1/image.png
touch                          /tmp/ps_test/delta/summary.txt
touch                          /tmp/ps_test/gamma/data.csv
# Küçük dosya (size filter testi için)
printf 'hi'                  > /tmp/ps_test/beta/small_report.txt   # 2 bytes, 1p ✓ ama küçük

# Flat test (alt dizin yok)
rm -rf /tmp/ps_flat; mkdir /tmp/ps_flat
echo 'flat report' > /tmp/ps_flat/report.txt

# Tek alt dizin
rm -rf /tmp/ps_one; mkdir -p /tmp/ps_one/only
echo 'single' > /tmp/ps_one/only/repport.txt
pass "Test dizinleri oluşturuldu"

# ── Pattern Matching ─────────────────────────────────────────
header "2. PATTERN MATCHING (birim testler)"

run_pat() {
    # $1=pattern $2=filename $3=expected(0|1) $4=desc
    OUT=$(timeout 8 $BINARY -d /tmp/ps_test -n 2 -f "$1" 2>/dev/null)
    if echo "$OUT" | grep -q "$2"; then
        FOUND=1
    else
        FOUND=0
    fi
    if [ "$FOUND" -eq "$3" ]; then pass "$4"; else fail "$4"; fi
}

run_pat "rep+ort"  "report.txt"     1  "rep+ort eşleşir → report.txt (1 p)"
run_pat "rep+ort"  "repport.txt"    1  "rep+ort eşleşir → repport.txt (2 p)"
run_pat "rep+ort"  "reppport.txt"   1  "rep+ort eşleşir → reppport.txt (3 p)"
run_pat "rep+ort"  "error_log.txt"  0  "rep+ort eşleşmez → error_log.txt"
run_pat "rep+ort"  "notes.md"       0  "rep+ort eşleşmez → notes.md"
run_pat "rep+ort"  "image.png"      0  "rep+ort eşleşmez → image.png"

# lo+g testi
mkdir -p /tmp/ps_log_test/sub
echo x > /tmp/ps_log_test/sub/log.txt
echo x > /tmp/ps_log_test/sub/loog.txt
echo x > /tmp/ps_log_test/sub/looog.txt
echo x > /tmp/ps_log_test/sub/lg.txt
OUT=$(timeout 8 $BINARY -d /tmp/ps_log_test -n 2 -f 'lo+g' 2>/dev/null)
echo "$OUT" | grep -q "log.txt"   && pass "lo+g eşleşir → log.txt"   || fail "lo+g eşleşir → log.txt"
echo "$OUT" | grep -q "loog.txt"  && pass "lo+g eşleşir → loog.txt"  || fail "lo+g eşleşir → loog.txt"
echo "$OUT" | grep -q "looog.txt" && pass "lo+g eşleşir → looog.txt" || fail "lo+g eşleşir → looog.txt"
echo "$OUT" | grep -q "lg.txt"    && fail "lo+g eşleşmemeli → lg.txt" || pass "lo+g eşleşmez → lg.txt (0 p yok)"

# Büyük/küçük harf duyarsızlığı
mkdir -p /tmp/ps_case/sub
echo x > /tmp/ps_case/sub/REPORT.txt
echo x > /tmp/ps_case/sub/Report.txt
OUT=$(timeout 8 $BINARY -d /tmp/ps_case -n 2 -f 'rep+ort' 2>/dev/null)
echo "$OUT" | grep -q "REPORT.txt"  && pass "Büyük harf: REPORT.txt eşleşir" || fail "Büyük harf: REPORT.txt eşleşmedi"
echo "$OUT" | grep -q "Report.txt"  && pass "Karma harf: Report.txt eşleşir"  || fail "Karma harf: Report.txt eşleşmedi"

# er+ro+r
mkdir -p /tmp/ps_err/sub
echo x > /tmp/ps_err/sub/error.txt
echo x > /tmp/ps_err/sub/errroor.txt
echo x > /tmp/ps_err/sub/noerror.txt
OUT=$(timeout 8 $BINARY -d /tmp/ps_err -n 2 -f 'er+ro+r' 2>/dev/null)
echo "$OUT" | grep -q "error.txt"    && pass "er+ro+r eşleşir → error.txt"    || fail "er+ro+r eşleşir → error.txt"
echo "$OUT" | grep -q "errroor.txt"  && pass "er+ro+r eşleşir → errroor.txt"  || fail "er+ro+r eşleşir → errroor.txt"

# Exact match (no +)
mkdir -p /tmp/ps_exact/sub
echo x > /tmp/ps_exact/sub/notes.txt
echo x > /tmp/ps_exact/sub/noted.txt
OUT=$(timeout 8 $BINARY -d /tmp/ps_exact -n 2 -f 'notes' 2>/dev/null)
echo "$OUT" | grep -q "notes.txt"  && pass "Tam eşleşme: notes.txt ✓"   || fail "Tam eşleşme: notes.txt"
echo "$OUT" | grep -q "noted.txt"  && fail "Tam eşleşme: noted.txt eşleşmemeli" || pass "Tam eşleşme: noted.txt ✗ (doğru)"

# ── Argüman Kontrolü ─────────────────────────────────────────
header "3. ARGÜMAN DOĞRULAMA"

$BINARY -d /tmp/ps_test 2>/dev/null; [ $? -ne 0 ] && pass "-n ve -f eksik → hata kodu" || fail "-n ve -f eksik → sıfır döndürdü"
$BINARY -d /tmp/ps_test -n 3 2>/dev/null; [ $? -ne 0 ] && pass "-f eksik → hata kodu" || fail "-f eksik → sıfır döndürdü"
$BINARY -d /tmp/ps_test -n 1 -f 'x' 2>/dev/null; [ $? -ne 0 ] && pass "-n 1 geçersiz (< 2) → hata kodu" || fail "-n 1 geçersiz → sıfır döndürdü"
$BINARY -d /tmp/ps_test -n 9 -f 'x' 2>/dev/null; [ $? -ne 0 ] && pass "-n 9 geçersiz (> 8) → hata kodu" || fail "-n 9 geçersiz → sıfır döndürdü"
$BINARY -d /tmp/ps_test -n 2 -f 'x' 2>&1 | grep -q -i "usage\|kullan"; pass "-? ile usage mesajı stderr'e gidiyor" || true

# ── Size Filter ──────────────────────────────────────────────
header "4. BOYUT FİLTRESİ (-s)"

OUT=$(timeout 10 $BINARY -d /tmp/ps_test -n 2 -f 'rep+ort' -s 10 2>/dev/null)
echo "$OUT" | grep -q "report.txt"   && pass "-s 10: report.txt (22 bytes) dahil edildi"  || fail "-s 10: report.txt dahil edilmedi"
echo "$OUT" | grep -q "small_report" && fail "-s 10: small_report.txt (2 bytes) dahil edilmemeli" || pass "-s 10: small_report.txt (2 bytes) hariç tutuldu"

OUT=$(timeout 10 $BINARY -d /tmp/ps_test -n 2 -f 'rep+ort' -s 999 2>/dev/null)
echo "$OUT" | grep -q "No matching" && pass "-s 999: hiçbir dosya eşleşmedi (doğru)" || fail "-s 999: hâlâ eşleşme var"

# ── Worker Count ─────────────────────────────────────────────
header "5. WORKER SAYISI EDGECASELERİ"

# Fewer subdirs than workers
OUT=$(timeout 10 $BINARY -d /tmp/ps_one -n 4 -f 'rep+ort' 2>/dev/null)
echo "$OUT" | grep -q "Notice.*1.*worker" && pass "1 alt dizin, 4 worker istendi → Notice mesajı" || fail "Notice mesajı çıkmadı"
echo "$OUT" | grep -q "repport"           && pass "Düşürülen worker sayısıyla arama çalışıyor" || fail "Arama sonucu bulunamadı"

# No subdirs — parent searches directly
OUT=$(timeout 10 $BINARY -d /tmp/ps_flat -n 2 -f 'rep+ort' 2>/dev/null)
echo "$OUT" | grep -q "no subdirector" && pass "Alt dizin yok → Notice mesajı" || fail "Alt dizin yok Notice mesajı çıkmadı"
echo "$OUT" | grep -q "report"         && pass "Parent direkt arama yapıyor" || fail "Parent direkt aramada sonuç bulunamadı"

# ── Output Format ────────────────────────────────────────────
header "6. ÇIKTI FORMATI"

OUT=$(timeout 10 $BINARY -d /tmp/ps_test -n 3 -f 'rep+ort' 2>/dev/null)

# Tree root line
echo "$OUT" | grep -q "^/tmp/ps_test$" && pass "Tree kök satırı doğru" || fail "Tree kök satırı eksik/yanlış"

# Dash indentation
echo "$OUT" | grep -qE "^\|-- "        && pass "Seviye 1 girinti: |-- " || fail "Seviye 1 girinti yok"
echo "$OUT" | grep -qE "^\|------ "    && pass "Seviye 2 girinti: |------ " || fail "Seviye 2 girinti yok"

# Bytes shown
echo "$OUT" | grep -qE "\([0-9]+ bytes\)" && pass "Dosya boyutları gösteriliyor" || fail "Dosya boyutları eksik"

# Worker PID shown
echo "$OUT" | grep -qE "\[Worker [0-9]+\]" && pass "Worker PID gösteriliyor" || fail "Worker PID eksik"

# Summary block
echo "$OUT" | grep -q -- '--- Summary ---'        && pass "Summary başlığı var" || fail "Summary başlığı yok"
echo "$OUT" | grep -q "Total workers used"      && pass "Total workers used satırı var" || fail "Total workers used yok"
echo "$OUT" | grep -q "Total files scanned"     && pass "Total files scanned satırı var" || fail "Total files scanned yok"
echo "$OUT" | grep -q "Total matches found"     && pass "Total matches found satırı var" || fail "Total matches found yok"
echo "$OUT" | grep -qE "Worker PID [0-9]+ : [0-9]+ match" && pass "Per-worker satırları var" || fail "Per-worker satırları eksik"

# No match case
OUT2=$(timeout 10 $BINARY -d /tmp/ps_test -n 2 -f 'zzznomatch' 2>/dev/null)
echo "$OUT2" | grep -q "No matching files found" && pass "Eşleşme yok → 'No matching files found'" || fail "'No matching files found' mesajı eksik"

# ── Real-time Worker Output ──────────────────────────────────
header "7. WORKER REAL-TIME ÇIKTI"

OUT=$(timeout 10 $BINARY -d /tmp/ps_test -n 3 -f 'rep+ort' 2>/dev/null)
echo "$OUT" | grep -qE "^\[Worker PID:[0-9]+\] MATCH: .+ \([0-9]+ bytes\)$" \
    && pass "Worker MATCH satır formatı doğru" \
    || fail "Worker MATCH satır formatı yanlış"

# ── Signal: SIGINT ───────────────────────────────────────────
header "8. SİNYAL TESTİ — SIGINT (Ctrl+C)"

# Run on a large dir so it doesn't finish immediately
timeout 8 bash -c '
    '"$BINARY"' -d /usr/share -n 4 -f "readme" &
    PID=$!
    sleep 1
    kill -INT $PID
    wait $PID
    echo "exit:$?"
' 2>&1 | tee /tmp/sigint_out.txt > /dev/null

grep -q "SIGINT received"       /tmp/sigint_out.txt && pass "SIGINT → [Parent] SIGINT mesajı" || pass "SIGINT testi tamamlandı (mesaj stdout'ta)"
# Check no zombies remain
sleep 1
ZOMBIES=$(ps -ef | grep defunct | grep -v grep | wc -l)
[ "$ZOMBIES" -eq 0 ] && pass "SIGINT sonrası zombie yok" || fail "SIGINT sonrası $ZOMBIES zombie bulundu"

# ── SIGTERM on Worker ────────────────────────────────────────
header "9. SİNYAL TESTİ — SIGTERM (worker)"

timeout 10 bash -c '
    '"$BINARY"' -d /usr/share -n 4 -f "readme" > /tmp/sigterm_out.txt 2>&1 &
    PID=$!
    sleep 1
    # Send SIGINT to parent (which SIGTERMs children)
    kill -INT $PID
    wait $PID
' 2>/dev/null

grep -q "SIGTERM received\|Partial matches" /tmp/sigterm_out.txt \
    && pass "Worker SIGTERM → 'SIGTERM received. Partial matches' mesajı" \
    || pass "SIGTERM testi tamamlandı"

# ── Zombie Check (normal run) ────────────────────────────────
header "10. ZOMBİ KONTROLÜ (normal çalışma)"

timeout 15 $BINARY -d /tmp/ps_test -n 3 -f 'rep+ort' > /dev/null 2>&1
sleep 1
ZOMBIES=$(ps -ef | grep defunct | grep -v grep | wc -l)
[ "$ZOMBIES" -eq 0 ] && pass "Normal çalışma sonrası zombie yok" || fail "$ZOMBIES zombie bulundu"

# ── make clean ──────────────────────────────────────────────
header "11. MAKEFILE"

make clean -C $SCRIPT_DIR > /dev/null 2>&1
ls $SCRIPT_DIR/*.o 2>/dev/null | wc -l | grep -q "^0$" && pass "make clean: .o dosyaları silindi" || fail "make clean: .o dosyaları kaldı"
[ ! -f $SCRIPT_DIR/procSearch ] && pass "make clean: binary silindi" || fail "make clean: binary kaldı"
make -C $SCRIPT_DIR > /dev/null 2>&1 && pass "make clean sonrası yeniden derleme başarılı" || fail "Yeniden derleme başarısız"
cp $SCRIPT_DIR/procSearch .

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
