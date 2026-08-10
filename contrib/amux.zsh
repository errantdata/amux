#compdef amux

typeset -A opt_args

_amux_sessions() {
  declare -a sessions
  sessions=( $(amux | sed '1d;s/.*\t[0-9][0-9]*\t//') )
  _describe -t session 'session' sessions
}

_amux_firstarg() {
  if (( $+opt_args[-a] || $+opt_args[-A] || $+opt_args[-H] )); then
    _amux_sessions
  elif (( $+opt_args[-c] || $+opt_args[-n] )); then
    _guard "^-*" 'session name'
  elif [[ -z $words[CURRENT] ]]; then
    compadd "$@" -S '' -- -
  fi
}

_arguments -s \
  '(-a -A -c -n -H -f)-a[attach to an existing session]' \
  '(-a -A -c -n -H)-A[attach to a session, create if does not exist]' \
  '(-a -A -c -n -H -l)-c[create a new session and attach to it]' \
  '(-a -A -c -n -H -l)-n[create a new session but do not attach to it]' \
  '(-a -A -c -n -H -e -f -p -q -r -l -s)-H[dump the full session history to stdout]' \
  '-e[set the detachkey (default: ^\\)]:detachkey' \
  '(-a)-f[force create the session]' \
  '(-q)-p[pass-through mode]' \
  '-q[be quiet]' \
  '-r[read-only session, ignore user input]' \
  '(-c -n)-l[attach with the lowest priority]' \
  '(-H)-s[history replayed on attach: lines|Nk|Nm|full|none]:replay:(full none 1000 10000 50000)' \
  '(-)-v[show version information and exit]' \
  '1: :_amux_firstarg' \
  '2:command:_path_commands' \
  '*:: :{ shift $((CURRENT-3)) words; _precommand; }'
