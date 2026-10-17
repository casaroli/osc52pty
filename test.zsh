#!/bin/zsh
# Unit tests for osc52pty.  Run: zsh test.zsh

emulate zsh
setopt nomultibyte
source ${0:A:h}/osc52pty || exit 1

tmp=$(mktemp -d) || exit 1
trap 'rm -rf $tmp' EXIT
pass=0 fail=0

check() {  # name expected actual
  if [[ $2 == "$3" ]]; then (( pass++ ))
  else
    (( fail++ ))
    print -r "FAIL $1"
    print -r "  expected: ${(q+)2}"
    print -r "  actual:   ${(q+)3}"
  fi
}

# record payloads instead of touching the clipboard
clip() { clips+=( "$1" ) }

# feed each argument as one chunk; $got is the output of each call joined
# by "|", $clipped the payloads joined by "|", $left what is still held back
feed() {
  local buf= out= c
  local -a steps
  clips=()
  for c in "$@"; do
    out=
    osc52_feed "$c"
    steps+=( "$out" )
  done
  got=${(j:|:)steps} clipped=${(j:|:)clips} left=$buf
}

t() {  # name expected-output expected-clips chunk...
  local name=$1 exp_out=$2 exp_clips=$3
  shift 3
  feed "$@"
  check "$name: output" "$exp_out" "$got"
  check "$name: clips" "$exp_clips" "$clipped"
  check "$name: nothing held" "" "$left"
}

print "== osc52_feed"
t "plain text"           'hello world' '' 'hello world'
t "empty chunk"          '' '' ''
t "BEL terminated"       'AB' 'c;aGk=' $'A\e]52;c;aGk=\aB'
t "ST terminated"        'AB' 'c;aGk=' $'A\e]52;c;aGk=\e\\B'
t "two in one chunk"     'ABC' 'c;MQ==|p;Mg==' $'A\e]52;c;MQ==\aB\e]52;p;Mg==\e\\C'
t "back to back"         '' 'c;MQ==|c;Mg==' $'\e]52;c;MQ==\a\e]52;c;Mg==\a'
t "BEL before ST"        $'B\e\\' 'c;QQ==' $'\e]52;c;QQ==\aB\e\\'
t "ST before BEL"        $'B\a' 'c;QQ==' $'\e]52;c;QQ==\e\\B\a'
t "query is passed on"   '' 'c;?' $'\e]52;c;?\a'

t "split after ESC"      'X|||Y' 'c;aGk=' $'X\e' ']5' '2;c;aGk=' $'\aY'
t "split byte by byte"   'X|||||||||||||Y' 'c;aGk=' X $'\e' ']' 5 2 ';' c ';' a G k = $'\a' Y
t "split in payload"     '|B' 'c;aGk=' $'\e]52;c;aG' $'k=\aB'
t "split inside ST"      'A|B' 'c;aGk=' $'A\e]52;c;aGk=\e' $'\\B'
t "chunk ends on start"  'A|B' 'c;aGk=' $'A\e]52;' $'c;aGk=\aB'

t "other OSC untouched"  $'\e]0;title\a' '' $'\e]0;title\a'
t "near miss \\e]5x"     $'\e]5x' '' $'\e]5x'
t "near miss \\e]520"    $'a|\e]520;x' '' $'a\e]52' '0;x'
t "held ESC was CSI"     $'a|\e[1mb' '' $'a\e' '[1mb'
t "ESC ESC ] 52"         $'\e' 'c;aGk=' $'\e\e]52;c;aGk=\a'
t "NUL bytes"            $'a\0b' '' $'a\0b'
t "UTF-8 split"          $'\xc3|\xa9' '' $'\xc3' $'\xa9'

feed $'\e]52;\a'
check "empty payload: one clip" 1 ${#clips}
check "empty payload: is empty" '' "$clips[1]"

feed $'a\e]52;c;aGk='
check "unterminated: output" a "$got"
check "unterminated: held" $'\e]52;c;aGk=' "$left"
check "unterminated: no clip" 0 ${#clips}

feed $'a\e]5'
check "partial start: output" a "$got"
check "partial start: held" $'\e]5' "$left"

osc52_max=10
feed $'\e]52;c;aGVsbG8gd29ybGQ=' 'x'
check "too long: given up" $'\e]52;c;aGVsbG8gd29ybGQ=|x' "$got"
check "too long: nothing held" '' "$left"
osc52_max=16777216

print "== clip"
unfunction clip
source ${0:A:h}/osc52pty
copy=( sh -c 'printf "<%s>" "$(cat)"' )
check "decodes"             '<hello>' "$(clip 'c;aGVsbG8=')"
check "primary selection"   '<hi>' "$(clip 'p;aGk=')"
check "multi selection"     '<hi>' "$(clip 'cp;aGk=')"
check "empty selection"     '<hi>' "$(clip ';aGk=')"
check "UTF-8"               '<wörld ✓>' "$(clip "c;$(print -rn 'wörld ✓' | base64)")"
check "query not copied"    '' "$(clip 'c;?')"
check "empty clears"        '<>' "$(clip 'c;')"
check "bad base64 is quiet" '<>' "$(clip 'c;!!!' 2>&1)"

print "== filter"
clip() { clips+=( "$1" ) }
filt() {  # input -> $got (stdout of filter), $clipped
  clips=()
  print -rn -- $1 | filter > $tmp/out
  got=$(<$tmp/out; print x) got=${got%x} clipped=${(j:|:)clips}
}
filt $'A\e]52;c;aGk=\aB'
check "pipe: output" AB "$got"
check "pipe: clips" 'c;aGk=' "$clipped"
filt $'a\e]52;c;aGk='
check "EOF flushes unterminated" $'a\e]52;c;aGk=' "$got"
filt $'a\e'
check "EOF flushes held ESC" $'a\e' "$got"

head -c 3000000 /dev/urandom > $tmp/rand
clips=()
filter < $tmp/rand > $tmp/rand.out
if (( ! $#clips )); then  # 1 in ~300000 that random data has a real sequence
  check "3MB random bytes pass unchanged" 0 "$(cmp -s $tmp/rand $tmp/rand.out; print $?)"
fi
{ head -c 100000 /dev/urandom | LC_ALL=C tr -d '\033'
  print -rn $'\e]52;c;aGk=\a'
  head -c 100000 /dev/urandom | LC_ALL=C tr -d '\033' } > $tmp/mixed
filter < $tmp/mixed > $tmp/mixed.out
check "sequence in 200KB of noise: clip" 'c;aGk=' "${(j:|:)clips}"
check "sequence in 200KB of noise: size" $(( $(wc -c < $tmp/mixed) - 12 )) $(wc -c < $tmp/mixed.out)

print "\n$pass passed, $fail failed"
(( fail == 0 ))
