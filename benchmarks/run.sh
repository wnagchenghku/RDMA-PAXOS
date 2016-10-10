define(){ IFS='\n' read -r -d '' ${1} || true; }
declare -A pids
redirection=( "> out" "2> err" "< /dev/null" )

define HELP <<'EOF'
Script for starting DARE
usage  : $0 [options]
options: --app                # app to run
         [--scount=INT]       # server count [default 3]
EOF

usage () {
    echo -e "$HELP"
}

ErrorAndExit () {
  echo "ERROR: $1"
  exit 1
}

Sync() {
    sed -i "s/group_size = .*/group_size = $1;/" ${DAREDIR}/target/nodes.local.cfg
    for ((i=0; i<$1; ++i));
    do
        cmd=( "scp" "${DAREDIR}/target/nodes.local.cfg" "$USER@${servers[$i]}:${DAREDIR}/target/" )
        $("${cmd[@]}")
        echo "Sync COMMAND: "${cmd[@]}
    done
}

StartDare() {
    for ((i=0; i<$1; ++i));
    do
        config_dare=( "server_type=start" "server_idx=$i" "config_path=${DAREDIR}/target/nodes.local.cfg" "dare_log_file=$PWD/srv${i}_1.log" "mgid=$DGID" "LD_PRELOAD=${DAREDIR}/target/interpose.so" )
        cmd=( "ssh" "$USER@${servers[$i]}" "${config_dare[@]}" "nohup" "${run_dare}" "${redirection[@]}" "&" "echo \$!" )
        pids[${servers[$i]}]=$("${cmd[@]}")
        echo "StartDare COMMAND: "${cmd[@]}
    done
    echo -e "\n\tinitial servers: ${!pids[@]}"
    echo -e "\t...and their PIDs: ${pids[@]}"
}

StopDare() {
    for i in "${!pids[@]}"
    do
        cmd=( "ssh" "$USER@$i" "kill -s SIGINT" "${pids[$i]}" )
        echo "Executing: ${cmd[@]}"
        $("${cmd[@]}")
    done
}

FindLeader() {
    leader=""
    max_idx=-1
    max_term=""
 
    for ((i=0; i<${group_size}; ++i)); do
        srv=${servers[$i]}
        # look for the latest [T<term>] LEADER 
        cmd=( "ssh" "$USER@$srv" "grep -r \"] LEADER\"" "$PWD/srv${i}_$((rounds[$srv]-1)).log" )
        #echo ${cmd[@]}
        grep_out=$("${cmd[@]}")
        if [[ -z $grep_out ]]; then
            continue
        fi
        terms=($(echo $grep_out | awk '{print $2}'))
        for j in "${terms[@]}"; do
           term=`echo $j | awk -F'T' '{print $2}' | awk -F']' '{print $1}'`
           if [[ $term -gt $max_term ]]; then 
                max_term=$term
                leader=$srv
                leader_idx=$i
           fi
        done
    done
    #echo "Leader: p${leader_idx} ($leader)"
}

StartBenchmark() {
	FindLeader
	if [[ "$APP" == "ssdb" ]]; then
		run_loop=( "${DAREDIR}/apps/ssdb/ssdb-master/tools/ssdb-bench" "$leader" "6379" "100" "4")
	elif [[ "$APP" == "redis" ]]; then
		run_loop=( "${DAREDIR}/apps/redis/install/bin/redis-benchmark" "-h $leader" "-n 100" "-c 4" "-t set,get" )
	
	cmd=( "ssh" "$USER@${client}" )
	$("${cmd[@]}")
	echo "Benchmark COMMDAND: ${cmd[@]}"
}

DAREDIR=$PWD/..
run_dare=""
server_count=3
APP=""
for arg in "$@"
do
    case ${arg} in
    --help|-help|-h)
        usage
        exit 1
        ;;
    --scount=*)
        server_count=`echo $arg | sed -e 's/--scount=//'`
        server_count=`eval echo ${server_count}`    # tilde and variable expansion
        ;;
    --app=*)
        APP=`echo $arg | sed -e 's/--app=//'`
        APP=`eval echo ${APP}`    # tilde and variable expansion
    esac
done

if [[ "x$APP" == "x" ]]; then
    ErrorAndExit "No app defined: --app"
elif [[ "$APP" == "ssdb" ]]; then
    run_dare="${DAREDIR}/apps/ssdb/ssdb-master/ssdb-server ${DAREDIR}/apps/ssdb/ssdb-master/ssdb.conf"
elif [[ "$APP" == "redis" ]]; then
    run_dare="${DAREDIR}/apps/redis/install/bin/redis-server"
fi


# list of allocated nodes, e.g., nodes=(n112002 n112001 n111902)
nodes=(10.22.1.1 10.22.1.2 10.22.1.3 10.22.1.4 10.22.1.5 10.22.1.6 10.22.1.7 10.22.1.8 10.22.1.9)
node_count=${#nodes[@]}
echo "Allocated ${node_count} nodes:" > nodes
for ((i=0; i<${node_count}; ++i)); do
    echo "$i:${nodes[$i]}" >> nodes
done

if [ $server_count -le 0 ]; then
    ErrorAndExit "0 < #servers; --scount"
fi

client=${nodes[$i]}
echo ">>> client: ${client}"

for ((i=0; i<${server_count}; ++i)); do
    servers[${i}]=${nodes[$i]}
done
echo ">>> ${server_count} servers: ${servers[@]}"

DGID="ff0e::ffff:e101:101"

########################################################################

Sync $server_count
echo -ne "Starting $server_count servers...\n"
StartDare $server_count
echo "done"

StartBenchmark

sleep 0.2
StopDare

########################################################################