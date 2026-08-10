#!/bin/sh

AMUX="./amux"
# set detach key explicitly in case it was changed in config.h
AMUX_OPTS="-e ^\\"

[ ! -z "$1" ] && AMUX="$1"
[ ! -x "$AMUX" ] && echo "usage: $0 /path/to/amux" && exit 1

# amux switches to passthrough mode when stdin is not a terminal, in which
# case it emits no prolog/epilog at all and every comparison below fails.
# Run this under a tty (CI uses tests/*.py instead, which allocate their own).
if [ ! -t 0 ]; then
	echo "$0: needs a terminal on stdin; run it from a tty"
	echo "(for headless/CI use: for t in tests/*.py; do python3 \$t; done)"
	exit 1
fi

TESTS_OK=0
TESTS_RUN=0

detach() {
	sleep 1
	printf ""
}

dvtm_cmd() {
	printf "$1\n"
	sleep 1
}

dvtm_session() {
	sleep 1
	dvtm_cmd 'c'
	dvtm_cmd 'c'
	dvtm_cmd 'c'
	sleep 1
	dvtm_cmd ' '
	dvtm_cmd ' '
	dvtm_cmd ' '
	sleep 1
	dvtm_cmd 'qq'
}

# $1 => session-name
expected_amux_prolog() {
	printf '\033[r\033[H\033[2J\033[22;2t\033]2;amux: %s\007' "$1"
}

# $1 => session-name, $2 => exit status
expected_amux_epilog() {
	printf '\033[?25h\033[0m\033[r\033[23;2t'
	printf 'amux: %s: session terminated with exit status %s\n' "$1" "$2"
}

# $1 => session-name, $2 => cmd to run
expected_amux_attached_output() {
	expected_amux_prolog "$1"
	$2
	expected_amux_epilog "$1" $?
}

# $1 => session-name, $2 => cmd to run
expected_amux_detached_output() {
	expected_amux_prolog "$1"
	$2 >/dev/null 2>&1
	expected_amux_epilog "$1" $?
}

check_environment() {
	[ "`$AMUX | wc -l`" -gt 1 ] && echo "amux session exists" && exit 1;
	pgrep amux && echo "amux process exists" && exit 1;
	return 0;
}

test_non_existing_command() {
	check_environment || return 1;
	$AMUX -c test ./non-existing-command >/dev/null 2>&1
	check_environment || return 1;
}

# $1 => session-name, $2 => command to execute
run_test_attached() {
	check_environment || return 1;

	local name="$1"
	local cmd="$2"
	local output="$name.out"
	local output_expected="$name.expected"

	TESTS_RUN=$((TESTS_RUN + 1))
	echo -n "Running test attached: $name "
	expected_amux_attached_output "$name" "$cmd" > "$output_expected" 2>&1

	if $AMUX -c "$name" $cmd 2>&1 | sed 's/.$//' > "$output" && sleep 1 &&
	   diff -u "$output_expected" "$output" && check_environment; then
		rm "$output" "$output_expected"
		TESTS_OK=$((TESTS_OK + 1))
		echo "OK"
		return 0
	else
		echo "FAIL"
		return 1
	fi
}

# $1 => session-name, $2 => command to execute
run_test_detached() {
	check_environment || return 1;

	local name="$1"
	local cmd="$2"
	local output="$name.out"
	local output_expected="$name.expected"

	TESTS_RUN=$((TESTS_RUN + 1))
	echo -n "Running test detached: $name "
	expected_amux_detached_output "$name" "$cmd" > "$output_expected" 2>&1

	if $AMUX -n "$name" $cmd >/dev/null 2>&1 && sleep 1 &&
	   $AMUX -a "$name" 2>&1 | sed 's/.$//' > "$output" &&
	   diff -u "$output_expected" "$output" && check_environment; then
		rm "$output" "$output_expected"
		TESTS_OK=$((TESTS_OK + 1))
		echo "OK"
		return 0
	else
		echo "FAIL"
		return 1
	fi
}

# $1 => session-name, $2 => command to execute
run_test_attached_detached() {
	check_environment || return 1;

	local name="$1"
	local cmd="$2"
	local output="$name.out"
	local output_expected="$name.expected"

	TESTS_RUN=$((TESTS_RUN + 1))
	echo -n "Running test: $name "
	$cmd >/dev/null 2>&1
	expected_amux_epilog "$name" $? > "$output_expected" 2>&1

	if detach | $AMUX $AMUX_OPTS -c "$name" $cmd >/dev/null 2>&1 && sleep 3 &&
	   $AMUX -a "$name" 2>&1 | tail -1 | sed 's/.$//' > "$output" &&
	   diff -u "$output_expected" "$output" && check_environment; then
		rm "$output" "$output_expected"
		TESTS_OK=$((TESTS_OK + 1))
		echo "OK"
		return 0
	else
		echo "FAIL"
		return 1
	fi
}

run_test_dvtm() {
	echo -n "Running dvtm test: "
	if ! which dvtm >/dev/null 2>&1; then
		echo "SKIPPED"
		return 0;
	fi

	TESTS_RUN=$((TESTS_RUN + 1))
	local name="dvtm"
	local output="$name.out"
	local output_expected="$name.expected"

	: > "$output_expected"
	if dvtm_session | $AMUX -c "$name" > "$output" 2>&1 &&
	   diff -u "$output_expected" "$output" && check_environment; then
		rm "$output" "$output_expected"
		TESTS_OK=$((TESTS_OK + 1))
		echo "OK"
		return 0
	else
		echo "FAIL"
		return 1
	fi
}

test_non_existing_command || echo "Execution of non existing command FAILED"

run_test_attached "awk" "awk 'BEGIN {for(i=1;i<=1000;i++) print i}'"
run_test_detached "awk" "awk 'BEGIN {for(i=1;i<=1000;i++) print i}'"

run_test_attached "false" "false"
run_test_detached "false" "false"

run_test_attached "true" "true"
run_test_detached "true" "true"

cat > exit-status.sh <<-EOT
	#!/bin/sh
	exit 42
EOT
chmod +x exit-status.sh

run_test_attached "exit-status" "./exit-status.sh"
run_test_detached "exit-status" "./exit-status.sh"

rm ./exit-status.sh

cat > long-running.sh <<-EOT
	#!/bin/sh
	echo Start
	date
	sleep 3
	echo Hello World
	sleep 3
	echo End
	date
	exit 1
EOT
chmod +x long-running.sh

run_test_attached_detached "attach-detach" "./long-running.sh"

rm ./long-running.sh

run_test_dvtm

[ $TESTS_OK -eq $TESTS_RUN ]
