#!/bin/bash
# ══════════════════════════════════════════════════════
#  aws-s3 — Full Test Suite
#  Run from the project root: bash run_tests.sh
# ══════════════════════════════════════════════════════

PASS=0; FAIL=0
SERVER_PID=""

# ── Colours ──────────────────────────────────────────
GREEN='\033[0;32m'; RED='\033[0;31m'; CYAN='\033[0;36m'; NC='\033[0m'

header() { echo -e "\n${CYAN}══ $1 ══${NC}"; }
ok()     { echo -e "  ${GREEN}PASS${NC}  $1"; ((PASS++)); }
fail()   { echo -e "  ${RED}FAIL${NC}  $1"; ((FAIL++)); }

check() {
    # check "label" <exit-code> [expected-string-in-output] [output]
    local label="$1" code="$2" expected="$3" output="$4"
    if [ "$code" -ne 0 ]; then
        fail "$label (exit $code)"
    elif [ -n "$expected" ] && ! echo "$output" | grep -q "$expected"; then
        fail "$label (expected '$expected' not found in output)"
    else
        ok "$label"
    fi
}

cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
    rm -rf buckets /tmp/aws-s3-test
}
trap cleanup EXIT

# ── Build ─────────────────────────────────────────────
header "Build"
make -s clean && make -s
check "make" $?

# ── Start server ──────────────────────────────────────
mkdir -p buckets
./aws-s3_server &>/tmp/aws-s3-server.log &
SERVER_PID=$!
sleep 0.4
kill -0 "$SERVER_PID" 2>/dev/null
check "server starts" $?

# ── Test data ─────────────────────────────────────────
mkdir -p /tmp/aws-s3-test/src/docs /tmp/aws-s3-test/src/imgs /tmp/aws-s3-test/dst
echo "Hello world"          > /tmp/aws-s3-test/src/hello.txt
echo "Report 2024"          > /tmp/aws-s3-test/src/docs/report.txt
echo "Meeting notes"        > /tmp/aws-s3-test/src/docs/notes.txt
printf '\x89PNG\r\n\x1a\n'  > /tmp/aws-s3-test/src/imgs/photo.png   # binary header
dd if=/dev/urandom bs=1024 count=64 of=/tmp/aws-s3-test/src/imgs/big.bin 2>/dev/null

# ══════════════════════════════════════════════════════
header "1. mb — make bucket"
OUT=$(./aws-s3 mb s3://test-bucket 2>&1); check "create bucket"            $? "make_bucket" "$OUT"
OUT=$(./aws-s3 mb s3://test-bucket 2>&1)
DUPE_EXIT=$?
[ $DUPE_EXIT -ne 0 ] && ok "duplicate bucket → error" || fail "duplicate bucket → error (should exit non-zero)"
OUT=$(./aws-s3 mb s3://other-bucket 2>&1); check "second bucket"           $? "make_bucket" "$OUT"

# ══════════════════════════════════════════════════════
header "2. ls — list buckets"
OUT=$(./aws-s3 ls 2>&1)
check "ls shows test-bucket"  0 "test-bucket"  "$OUT"
check "ls shows other-bucket" 0 "other-bucket" "$OUT"

# ══════════════════════════════════════════════════════
header "3. cp — upload single file"
OUT=$(./aws-s3 cp /tmp/aws-s3-test/src/hello.txt s3://test-bucket/ 2>&1)
check "upload text file"     $? "hello.txt" "$OUT"

OUT=$(./aws-s3 cp /tmp/aws-s3-test/src/imgs/photo.png s3://test-bucket/imgs/photo.png 2>&1)
check "upload binary (PNG)"  $? "photo.png" "$OUT"

OUT=$(./aws-s3 cp /tmp/aws-s3-test/src/imgs/big.bin s3://test-bucket/big.bin 2>&1)
check "upload 64 KB binary"  $? "big.bin" "$OUT"

# ══════════════════════════════════════════════════════
header "4. ls — list bucket contents"
OUT=$(./aws-s3 ls s3://test-bucket/ 2>&1)
check "ls shows hello.txt"   0 "hello.txt"       "$OUT"
check "ls shows photo.png"   0 "imgs/photo.png"  "$OUT"
check "ls shows big.bin"     0 "big.bin"          "$OUT"

OUT=$(./aws-s3 ls s3://test-bucket/imgs/ 2>&1)
check "ls with prefix filter" 0 "photo.png" "$OUT"

# ══════════════════════════════════════════════════════
header "5. cp — download single file"
OUT=$(./aws-s3 cp s3://test-bucket/hello.txt /tmp/aws-s3-test/dst/hello.txt 2>&1)
check "download text file"         $? "" "$OUT"
[ -f /tmp/aws-s3-test/dst/hello.txt ] && \
  diff /tmp/aws-s3-test/src/hello.txt /tmp/aws-s3-test/dst/hello.txt &>/dev/null
check "downloaded content matches" $?

OUT=$(./aws-s3 cp s3://test-bucket/big.bin /tmp/aws-s3-test/dst/big.bin 2>&1)
check "download 64 KB binary"      $? "" "$OUT"
diff /tmp/aws-s3-test/src/imgs/big.bin /tmp/aws-s3-test/dst/big.bin &>/dev/null
check "binary content is exact"    $?

# ══════════════════════════════════════════════════════
header "6. cp — S3 to S3 copy"
OUT=$(./aws-s3 cp s3://test-bucket/hello.txt s3://other-bucket/copy-of-hello.txt 2>&1)
check "S3→S3 copy"                  $? "" "$OUT"
OUT=$(./aws-s3 ls s3://other-bucket/ 2>&1)
check "copy appears in other-bucket" 0 "copy-of-hello.txt" "$OUT"

# ══════════════════════════════════════════════════════
header "7. cp --recursive"
OUT=$(./aws-s3 cp /tmp/aws-s3-test/src/ s3://test-bucket/backup/ --recursive 2>&1)
check "recursive upload"             $? "" "$OUT"
OUT=$(./aws-s3 ls s3://test-bucket/ 2>&1)
check "recursive: docs/report.txt uploaded" 0 "docs/report.txt" "$OUT"
check "recursive: docs/notes.txt uploaded"  0 "docs/notes.txt"  "$OUT"

mkdir -p /tmp/aws-s3-test/recursive-dst
OUT=$(./aws-s3 cp s3://test-bucket/backup/ /tmp/aws-s3-test/recursive-dst/ --recursive 2>&1)
check "recursive download"           $? "" "$OUT"
[ -f /tmp/aws-s3-test/recursive-dst/docs/report.txt ]
check "recursive: nested file on disk" $?

# ══════════════════════════════════════════════════════
header "8. mv — move file"
echo "to be moved" > /tmp/aws-s3-test/move-me.txt
./aws-s3 cp /tmp/aws-s3-test/move-me.txt s3://test-bucket/move-me.txt &>/dev/null

OUT=$(./aws-s3 mv s3://test-bucket/move-me.txt s3://other-bucket/moved.txt 2>&1)
check "S3→S3 move"                  $? "" "$OUT"
OUT=$(./aws-s3 ls s3://other-bucket/ 2>&1)
check "file appears at destination"  0 "moved.txt" "$OUT"
OUT=$(./aws-s3 ls s3://test-bucket/ 2>&1)
echo "$OUT" | grep -qv "move-me.txt"
check "file gone from source"        $?

# ══════════════════════════════════════════════════════
header "9. rm — delete object"
OUT=$(./aws-s3 rm s3://test-bucket/hello.txt 2>&1)
check "rm single object"             $? "" "$OUT"
OUT=$(./aws-s3 ls s3://test-bucket/ 2>&1)
echo "$OUT" | grep -qv "hello.txt"
check "object gone from listing"     $?

# rm --recursive
OUT=$(./aws-s3 rm s3://test-bucket/backup/ --recursive 2>&1)
check "rm --recursive"               $? "" "$OUT"
OUT=$(./aws-s3 ls s3://test-bucket/ 2>&1)
echo "$OUT" | grep -qv "backup/docs"
check "prefix entirely removed"      $?

# ══════════════════════════════════════════════════════
header "10. sync"
mkdir -p /tmp/aws-s3-test/sync-src
echo "Alpha"   > /tmp/aws-s3-test/sync-src/alpha.txt
echo "Beta"    > /tmp/aws-s3-test/sync-src/beta.txt

./aws-s3 mb s3://sync-bucket &>/dev/null

OUT=$(./aws-s3 sync /tmp/aws-s3-test/sync-src/ s3://sync-bucket/ 2>&1)
check "sync first run uploads all"   $? "2 uploaded" "$OUT"

OUT=$(./aws-s3 sync /tmp/aws-s3-test/sync-src/ s3://sync-bucket/ 2>&1)
check "sync second run skips all"    $? "0 uploaded" "$OUT"

echo "Alpha v2 — changed" > /tmp/aws-s3-test/sync-src/alpha.txt
OUT=$(./aws-s3 sync /tmp/aws-s3-test/sync-src/ s3://sync-bucket/ 2>&1)
check "sync re-uploads changed file" $? "1 uploaded" "$OUT"

rm /tmp/aws-s3-test/sync-src/beta.txt
OUT=$(./aws-s3 sync /tmp/aws-s3-test/sync-src/ s3://sync-bucket/ --delete 2>&1)
check "sync --delete removes orphan" $? "1 deleted" "$OUT"
OUT=$(./aws-s3 ls s3://sync-bucket/ 2>&1)
echo "$OUT" | grep -qv "beta.txt"
check "beta.txt gone after --delete" $?

# ══════════════════════════════════════════════════════
header "11. rb — remove bucket"
OUT=$(./aws-s3 rb s3://other-bucket 2>&1)
check "rb non-empty bucket → error"  $? "" "$OUT"
[[ "$OUT" == *"Error"* ]] || { fail "should error on non-empty"; ((PASS--)); }

./aws-s3 rb s3://other-bucket --force &>/dev/null
check "rb --force removes bucket"    $?
OUT=$(./aws-s3 ls 2>&1)
echo "$OUT" | grep -qv "other-bucket"
check "bucket gone from ls"          $?

# ══════════════════════════════════════════════════════
header "12. Free space reuse"
./aws-s3 mb s3://freespace-test &>/dev/null
echo "AAAAAAAAAA" > /tmp/aws-s3-test/v1.txt   # 11 bytes
./aws-s3 cp /tmp/aws-s3-test/v1.txt s3://freespace-test/file.txt &>/dev/null

echo "BBBBB"      > /tmp/aws-s3-test/v2.txt   # 6 bytes (smaller — triggers free slot)
./aws-s3 cp /tmp/aws-s3-test/v2.txt s3://freespace-test/file.txt &>/dev/null

OUT=$(./aws-s3 cp s3://freespace-test/file.txt /tmp/aws-s3-test/got.txt 2>&1)
check "overwrite smaller file"   $? "" "$OUT"
CONTENT=$(cat /tmp/aws-s3-test/got.txt 2>/dev/null)
[ "$CONTENT" = "BBBBB" ]
check "content is the new version" $?

# same-size overwrite (in-place)
echo "CCCCC" > /tmp/aws-s3-test/v3.txt
./aws-s3 cp /tmp/aws-s3-test/v3.txt s3://freespace-test/file.txt &>/dev/null
./aws-s3 cp s3://freespace-test/file.txt /tmp/aws-s3-test/got2.txt &>/dev/null
CONTENT=$(cat /tmp/aws-s3-test/got2.txt 2>/dev/null)
[ "$CONTENT" = "CCCCC" ]
check "same-size in-place overwrite" $?

# ══════════════════════════════════════════════════════
echo ""
echo "══════════════════════════════════════════════════"
echo -e "  Results: ${GREEN}${PASS} passed${NC}  ${RED}${FAIL} failed${NC}"
echo "══════════════════════════════════════════════════"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
