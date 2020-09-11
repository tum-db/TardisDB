#!/bin/bash

if [[ $platform == 'linux' ]]; then
    Color_Off='\033[0m'
    Green='\033[0;32m'
    Yellow='\033[0;33m'
    Red='\033[0;31m'
else
    Color_Off=''
    Green=''
    Yellow=''
    Red=''
fi

COMMIT_ID=$(git rev-parse --verify HEAD)

benchmark_input() {
    # Execute benchmark program and write output to file
    (./semanticalBench "-d=$5" "-r=$4" < $1) | cat > output.txt

    # Declare metric arrays
    declare -a parsing_times
    declare -a analysing_times
    declare -a translation_times
    declare -a compile_times
    declare -a execution_times
    declare -a sums

    declare -a time_sec
    declare -a cycles
    declare -a instructions
    declare -a l1_misses
    declare -a llc_misses
    declare -a branch_misses
    declare -a task_clock
    declare -a scale
    declare -a ipc
    declare -a cpus
    declare -a ghz

    # Retrieve metrics from file
    input="output.txt"
    #lineCounter=0
    #insertLimit=$6
    while IFS= read -r line
    do
        # Skip all insert and update statements which are responsible for loading the data into storage
        #((lineCounter=lineCounter+1))
        #if [ $lineCounter -le $(($insertLimit)) ]; then
        #      continue
        #fi

        IFS=','
        read -ra METRICS <<< "$line"
        if [ "${#METRICS[@]}" = "6" ]; then
            for i in "${!METRICS[@]}"; do
                case "$i" in
                    0)   parsing_times+=("$(echo -e "${METRICS[i]}" | tr -d '[:space:]')") ;;
                    1)   analysing_times+=("$(echo -e "${METRICS[i]}" | tr -d '[:space:]')") ;;
                    2)   translation_times+=("$(echo -e "${METRICS[i]}" | tr -d '[:space:]')") ;;
                    3)   compile_times+=("$(echo -e "${METRICS[i]}" | tr -d '[:space:]')") ;;
                    4)   execution_times+=("$(echo -e "${METRICS[i]}" | tr -d '[:space:]')") ;;
                    5)   sums+=("$(echo -e "${METRICS[i]}" | tr -d '[:space:]')") ;;
                esac
            done
        fi
        if [ "${#METRICS[@]}" = "11" ]; then
            for i in "${!METRICS[@]}"; do
                case "$i" in
                    0)   time_sec+=("$(echo -e "${METRICS[i]}" | tr -d '[:space:]')") ;;
                    1)   cycles+=("$(echo -e "${METRICS[i]}" | tr -d '[:space:]')") ;;
                    2)   instructions+=("$(echo -e "${METRICS[i]}" | tr -d '[:space:]')") ;;
                    3)   l1_misses+=("$(echo -e "${METRICS[i]}" | tr -d '[:space:]')") ;;
                    4)   llc_misses+=("$(echo -e "${METRICS[i]}" | tr -d '[:space:]')") ;;
                    5)   branch_misses+=("$(echo -e "${METRICS[i]}" | tr -d '[:space:]')") ;;
                    6)   task_clock+=("$(echo -e "${METRICS[i]}" | tr -d '[:space:]')") ;;
                    7)   scale+=("$(echo -e "${METRICS[i]}" | tr -d '[:space:]')") ;;
                    8)   ipc+=("$(echo -e "${METRICS[i]}" | tr -d '[:space:]')") ;;
                    9)   cpus+=("$(echo -e "${METRICS[i]}" | tr -d '[:space:]')") ;;
                    10)   ghz+=("$(echo -e "${METRICS[i]}" | tr -d '[:space:]')") ;;
                esac
            done
        fi
    done < "$input"

    #Cleanup
    rm output.txt

    # Sum up all same metrics
    parsing_time=0
    analysing_time=0
    translation_time=0
    compile_time=0
    execution_time=0
    sum=0
    counter=0

    TIME_SEC=0
    CYCLES=0
    INSTRUCTIONS=0
    L1_MISSES=0
    LLC_MISSES=0
    BRANCH_MISSES=0
    TASK_CLOCK=0
    SCALE=0
    IPC=0
    CPUS=0
    GHZ=0

    for i in "${!parsing_times[@]}"; do
        parsing_time=$(bc -l <<<"${parsing_time}+${parsing_times[i]}")
        analysing_time=$(bc -l <<<"${analysing_time}+${analysing_times[i]}")
        translation_time=$(bc -l <<<"${translation_time}+${translation_times[i]}")
        compile_time=$(bc -l <<<"${compile_time}+${compile_times[i]}")
        execution_time=$(bc -l <<<"${execution_time}+${execution_times[i]}")
        sum=$(bc -l <<<"${sum}+${sums[i]}")
        counter=$(bc -l <<<"${counter}+1")
    done

    parsing_time=$(bc -l <<<"${parsing_time}/${counter}")
    analysing_time=$(bc -l <<<"${analysing_time}/${counter}")
    translation_time=$(bc -l <<<"${translation_time}/${counter}")
    compile_time=$(bc -l <<<"${compile_time}/${counter}")
    execution_time=$(bc -l <<<"${execution_time}/${counter}")
    sum=$(bc -l <<<"${sum}/${counter}")

    counter=0
    for i in "${!time_sec[@]}"; do
        TIME_SEC=$(bc -l <<<"${TIME_SEC}+${time_sec[i]}")
        CYCLES=$(bc -l <<<"${CYCLES}+${cycles[i]}")
        INSTRUCTIONS=$(bc -l <<<"${INSTRUCTIONS}+${instructions[i]}")
        L1_MISSES=$(bc -l <<<"${L1_MISSES}+${l1_misses[i]}")
        LLC_MISSES=$(bc -l <<<"${LLC_MISSES}+${llc_misses[i]}")
        BRANCH_MISSES=$(bc -l <<<"${BRANCH_MISSES}+${branch_misses[i]}")
        TASK_CLOCK=$(bc -l <<<"${TASK_CLOCK}+${task_clock[i]}")
        SCALE=$(bc -l <<<"${SCALE}+${scale[i]}")
        IPC=$(bc -l <<<"${IPC}+${ipc[i]}")
        CPUS=$(bc -l <<<"${CPUS}+${cpus[i]}")
        GHZ=$(bc -l <<<"${GHZ}+${ghz[i]}")
        counter=$(bc -l <<<"${counter}+1")
    done

    TIME_SEC=$(bc -l <<<"${TIME_SEC}/${counter}")
    CYCLES=$(bc -l <<<"${CYCLES}/${counter}")
    INSTRUCTIONS=$(bc -l <<<"${INSTRUCTIONS}/${counter}")
    L1_MISSES=$(bc -l <<<"${L1_MISSES}/${counter}")
    LLC_MISSES=$(bc -l <<<"${LLC_MISSES}/${counter}")
    BRANCH_MISSES=$(bc -l <<<"${BRANCH_MISSES}/${counter}")
    TASK_CLOCK=$(bc -l <<<"${TASK_CLOCK}/${counter}")
    SCALE=$(bc -l <<<"${SCALE}/${counter}")
    IPC=$(bc -l <<<"${IPC}/${counter}")
    CPUS=$(bc -l <<<"${CPUS}/${counter}")
    GHZ=$(bc -l <<<"${GHZ}/${counter}")

    # Append metrics to csv file
    echo "$3;$5;${parsing_time};${analysing_time};${translation_time};${compile_time};${execution_time};${sum};${TIME_SEC};${CYCLES};${INSTRUCTIONS};${L1_MISSES};${LLC_MISSES};${BRANCH_MISSES};${TASK_CLOCK};${SCALE};${IPC};${CPUS};${GHZ}" | cat >> $2
}

<<STATEMENTS
1: MS = SELECT id FROM revision r WHERE r.pageId = <pageid>;
2: BS = SELECT id FROM revision VERSION branch1 r WHERE r.pageId = <pageid>;
3: MM = SELECT title FROM page p , revision r , content c WHERE p.id = r.pageId AND r.textId = c.id AND r.pageId = <pageid>;
4: BM = SELECT title FROM page p , revision VERSION branch1 r , content VERSION branch1 c WHERE r.textId = c.id AND p.id = r.pageId AND r.pageId = <pageid>;
5: MU = UPDATE revision SET parentid = 1 WHERE r.pageId = <pageid>;
6: BU = UPDATE revision VERSION branch1 SET parentid = 1 WHERE r.pageId = <pageid>;
7: MI = INSERT INTO content ( id , text ) VALUES ( <textid> , 'Hello_world!' );
8: BI = INSERT INTO content VERSION branch1 ( id , text ) VALUES (<textid> , 'Hello_World!');
9: MD = DELETE FROM revision WHERE pageId = <pageId>;
10: BD = DELETE FROM revision VERSION branch1 WHERE pageId = <pageId>;
STATEMENTS

generate_MS() {
    rm ms_statements.txt
    for i in {1..25}
    do
        echo "SELECT textId FROM page p WHERE p.id = $(( ( RANDOM % 30303 )  + 1 ));" | cat >> ms_statements.txt
    done
    echo "quit" | cat >> ms_statements.txt
}

generate_BS() {
    rm bs_statements.txt
    for i in {1..25}
    do
        echo "SELECT textId FROM page VERSION branch1 p WHERE p.id = $(( ( RANDOM % 30303 )  + 1 ));" | cat >> bs_statements.txt
    done
    echo "quit" | cat >> bs_statements.txt
}

generate_MM() {
    rm mm_statements.txt
    for i in {1..25}
    do
        echo "SELECT text FROM page p , content c WHERE p.textId = c.id AND p.id = $(( ( RANDOM % 30303 )  + 1 ));" | cat >> mm_statements.txt
    done
    echo "quit" | cat >> mm_statements.txt
}

generate_BM() {
    rm bm_statements.txt
    for i in {1..25}
    do
        echo "SELECT text FROM page VERSION branch1 p , content VERSION branch1 c WHERE p.textId = c.id AND p.id = $(( ( RANDOM % 30303 )  + 1 ));" | cat >> bm_statements.txt
    done
    echo "quit" | cat >> bm_statements.txt
}

generate_MU() {
    rm mu_statements.txt
    for i in {1..25}
    do
        echo "UPDATE page SET textId = 1 WHERE id = $(( ( RANDOM % 30303 )  + 1 ));" | cat >> mu_statements.txt
    done
    echo "quit" | cat >> mu_statements.txt
}

generate_BU() {
    rm bu_statements.txt
    for i in {1..25}
    do
        echo "UPDATE page VERSION branch1 SET textId = 1 WHERE id = $(( ( RANDOM % 30303 )  + 1 ));" | cat >> bu_statements.txt
    done
    echo "quit" | cat >> bu_statements.txt
}

generate_MI() {
    rm mi_statements.txt
    for i in {1..25}
    do
        echo "INSERT INTO content ( id , text ) VALUES ( $(( ( RANDOM % 30303 )  + 1 )) , 'Hello_world!' );" | cat >> mi_statements.txt
    done
    echo "quit" | cat >> mi_statements.txt
}

generate_BI() {
    rm bi_statements.txt
    for i in {1..25}
    do
        echo "INSERT INTO content VERSION branch1 ( id , text ) VALUES ( $(( ( RANDOM % 30303 )  + 1 )) , 'Hello_World!');" | cat >> bi_statements.txt
    done
    echo "quit" | cat >> bi_statements.txt
}

generate_MD() {
    rm md_statements.txt
    for i in {1..25}
    do
        echo "DELETE FROM page WHERE id = $(( ( RANDOM % 30303 )  + 1 )) ;" | cat >> md_statements.txt
    done
    echo "quit" | cat >> md_statements.txt
}

generate_BD() {
    rm bd_statements.txt
    for i in {1..25}
    do
        echo "DELETE FROM page VERSION branch1 WHERE id = $(( ( RANDOM % 30303 )  + 1 )) ;" | cat >> bd_statements.txt
    done
    echo "quit" | cat >> bd_statements.txt
}

benchmark_input_for_distributions() {
#    cat insert_statements.txt | cat > buffer_file.txt
#    cat $1 | cat >> buffer_file.txt

#    IFS=' '
#    read -ra LineCountInfo <<< "$(wc -l insert_statements.txt)"
#    insertLimit=${LineCountInfo[0]}
#    insertLimit=$(bc -l <<<"${insertLimit}*2")
#    insertLimit=$(bc -l <<<"${insertLimit}+1")

#    benchmark_input $1 $2 $3 $4 "0.9999"
#    benchmark_input $1 $2 $3 $4 "0.999"
#    benchmark_input $1 $2 $3 $4 "0.99"
#    benchmark_input $1 $2 $3 $4 "0.9"
    benchmark_input $1 $2 $3 $4 "0.5" #$insertLimit
#    benchmark_input $1 $2 $3 $4 "0.1"
#    benchmark_input $1 $2 $3 $4 "0.01"
#    benchmark_input $1 $2 $3 $4 "0.001"
#    benchmark_input $1 $2 $3 $4 "0.0001"

#    rm buffer_file.txt
}

OUTPUT_FILE=$(echo "../benchmarkResults/results_${COMMIT_ID}.csv")
rm $OUTPUT_FILE
echo "Type;Dist;ParsingTime;AnalysingTime;TranslationTime;CompilationTime;ExecutionTime;Time;TimeSec;Cycles;Instructions;L1Misses;LLCMisses;BranchMisses;TaskClock;Scale;IPC;CPUS;GHZ" | cat > $OUTPUT_FILE

generate_MS
generate_BS
echo "${Green}Generated Select Statements!${Color_Off}"
generate_MM
generate_BM
echo "${Green}Generated Merge Statements!${Color_Off}"
generate_MU
generate_BU
echo "${Green}Generated Update Statements!${Color_Off}"
generate_MI
generate_BI
echo "${Green}Generated Insert Statements!${Color_Off}"
generate_MD
generate_BD
echo "${Green}Generated Delete Statements!${Color_Off}"

echo ""
echo "Benchmark Select Statements..."
benchmark_input_for_distributions ms_statements.txt $OUTPUT_FILE 1 3
echo "Benchmark Select Statements with branching..."
benchmark_input_for_distributions bs_statements.txt $OUTPUT_FILE 2 3
echo "Benchmark Merge Statements..."
benchmark_input_for_distributions mm_statements.txt $OUTPUT_FILE 3 3
echo "Benchmark Merge Statements with branching..."
benchmark_input_for_distributions bm_statements.txt $OUTPUT_FILE 4 3
echo "Benchmark Update Statements..."
benchmark_input_for_distributions mu_statements.txt $OUTPUT_FILE 5 3
echo "Benchmark Update Statements with branching..."
benchmark_input_for_distributions bu_statements.txt $OUTPUT_FILE 6 3
echo "Benchmark Insert Statements..."
benchmark_input mi_statements.txt $OUTPUT_FILE 7 1 "0.5"
echo "Benchmark Insert Statements with branching..."
benchmark_input bi_statements.txt $OUTPUT_FILE 8 1 "0.5"
echo "Benchmark Delete Statements..."
benchmark_input_for_distributions md_statements.txt $OUTPUT_FILE 9 1
echo "Benchmark Delete Statements with branching..."
benchmark_input_for_distributions bd_statements.txt $OUTPUT_FILE 10 1
