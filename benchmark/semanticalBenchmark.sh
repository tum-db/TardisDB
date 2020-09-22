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
    (./semanticalBench "-d=$5" "-r=$4" "--lowerBound=$6" "--upperBound=$7" < $1) | cat > output.txt

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
    declare -a loadDuration
    declare -a pageSize
    declare -a userSize

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
        if [ "${#METRICS[@]}" = "1" ]; then
            loadDurationCandidate=$(echo ${METRICS[0]} | grep 'LoadDuration' | cut -f2 -d ":")
            loadDurationCandidate=$(echo -e "$loadDurationCandidate" | tr -d '[:space:]')
            if [[ $loadDurationCandidate != "" ]]; then
                loadDuration=$(echo $loadDurationCandidate)
            fi
            pageSizeCandidate=$(echo ${METRICS[0]} | grep 'Page' | cut -f2 -d ":")
            pageSizeCandidate=$(echo -e "$pageSizeCandidate" | tr -d '[:space:]')
            if [[ $pageSizeCandidate != "" ]]; then
                pageSize=$(echo $pageSizeCandidate)
            fi
            userSizeCandidate=$(echo ${METRICS[0]} | grep 'User' | cut -f2 -d ":")
            userSizeCandidate=$(echo -e "$userSizeCandidate" | tr -d '[:space:]')
            if [[ $userSizeCandidate != "" ]]; then
                userSize=$(echo $userSizeCandidate)
            fi
        fi
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

    if [ $counter -gt 0 ]; then
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
    fi

    # Append metrics to csv file
    echo "$3;$5;$loadDuration;$pageSize;$userSize;${parsing_time};${analysing_time};${translation_time};${compile_time};${execution_time};${sum};${TIME_SEC};${CYCLES};${INSTRUCTIONS};${L1_MISSES};${LLC_MISSES};${BRANCH_MISSES};${TASK_CLOCK};${SCALE};${IPC};${CPUS};${GHZ}" | cat >> $2
}

<<STATEMENTS
1: MS = SELECT content FROM page p WHERE p.id = <pageid>;
2: B1S = SELECT content FROM page VERSION branch1 p WHERE p.id = <pageid>;
3: B2S = SELECT content FROM page VERSION branch2 p WHERE p.id = <pageid>;
4: MM = SELECT name FROM user u , page p WHERE p.userId = u.id AND p.id = <pageid>;
7: B1M = SELECT name FROM user u , page VERSION branch1 p WHERE p.userId = u.id AND p.id = <pageid>;
8: B2M = SELECT name FROM user u , page VERSION branch2 p WHERE p.userId = u.id AND p.id = <pageid>;
9: MU = UPDATE page SET content = 'Hello_World!' WHERE id = <pageid>;
10: B1U = UPDATE page VERSION branch1 SET content = 'Hello_World!' WHERE id = <pageid>;
11: B2U = UPDATE page VERSION branch2 SET content = 'Hello_World!' WHERE id = <pageid>;
12: MI = INSERT INTO content ( id , text ) VALUES ( <textid> , 'Hello_world!' );
13: BI = INSERT INTO content VERSION branch1 ( id , text ) VALUES (<textid> , 'Hello_World!');
14: MD = DELETE FROM page WHERE id = <pageId>;
15: B1D = DELETE FROM page VERSION branch1 WHERE id = <pageId>;
16: B2D = DELETE FROM page VERSION branch2 WHERE id = <pageId>;
STATEMENTS

generate_MS() {
    statementFile=$(echo "./benchmarkStatements/ms_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..25}
    do
        echo "SELECT content FROM page p WHERE p.id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_B1S() {
    statementFile=$(echo "./benchmarkStatements/b1s_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..25}
    do
        echo "SELECT content FROM page VERSION branch1 p WHERE p.id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_B2S() {
    statementFile=$(echo "./benchmarkStatements/b2s_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..25}
    do
        echo "SELECT content FROM page VERSION branch2 p WHERE p.id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_MM() {
    statementFile=$(echo "./benchmarkStatements/mm_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..25}
    do
        echo "SELECT name FROM user u , page p WHERE p.userId = u.id AND p.id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_B1M() {
    statementFile=$(echo "./benchmarkStatements/b1m_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..25}
    do
        echo "SELECT name FROM user u , page VERSION branch1 p WHERE p.userId = u.id AND p.id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_B2M() {
    statementFile=$(echo "./benchmarkStatements/b2m_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..25}
    do
        echo "SELECT name FROM user u , page VERSION branch2 p WHERE p.userId = u.id AND p.id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_MU() {
    statementFile=$(echo "./benchmarkStatements/mu_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..25}
    do
        echo "UPDATE page SET content = 'Hello_World!' WHERE id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_B1U() {
    statementFile=$(echo "./benchmarkStatements/b1u_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..25}
    do
        echo "UPDATE page VERSION branch1 SET content = 'Hello_World!' WHERE id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_B2U() {
    statementFile=$(echo "./benchmarkStatements/b2u_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..25}
    do
        echo "UPDATE page VERSION branch2 SET content = 'Hello_World!' WHERE id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_MI() {
    statementFile=$(echo "./benchmarkStatements/mi_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..25}
    do
        echo "INSERT INTO user ( id , name ) VALUES ( $(( ( RANDOM % ($2 - $1) ) + $1 + 1 )) , 'John_Doe' );" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_BI() {
    statementFile=$(echo "./benchmarkStatements/bi_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..25}
    do
        echo "INSERT INTO user VERSION branch1 ( id , name ) VALUES ( $(( ( RANDOM % ($2 - $1) ) + $1 + 1 )) , 'John_Doe');" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_MD() {
    statementFile=$(echo "./benchmarkStatements/md_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..25}
    do
        echo "DELETE FROM page WHERE id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 )) ;" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_B1D() {
    statementFile=$(echo "./benchmarkStatements/b1d_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..25}
    do
        echo "DELETE FROM page VERSION branch1 WHERE id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 )) ;" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_B2D() {
    statementFile=$(echo "./benchmarkStatements/b2d_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..25}
    do
        echo "DELETE FROM page VERSION branch2 WHERE id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 )) ;" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_statements() {
    generate_MS $1 $2
    generate_B1S $1 $2
    generate_B2S $1 $2
    echo "${Green}Generated Select Statements for PageIDs $1 to $2!${Color_Off}"
    generate_MM $1 $2
    generate_B1M $1 $2
    generate_B2M $1 $2
    echo "${Green}Generated Merge Statements for PageIDs $1 to $2!${Color_Off}"
    generate_MU $1 $2
    generate_B1U $1 $2
    generate_B2U $1 $2
    echo "${Green}Generated Update Statements for PageIDs $1 to $2!${Color_Off}"
    generate_MI $1 $2
    generate_BI $1 $2
    echo "${Green}Generated Insert Statements for PageIDs $1 to $2!${Color_Off}"
    generate_MD $1 $2
    generate_B1D $1 $2
    generate_B2D $1 $2
    echo "${Green}Generated Delete Statements for PageIDs $1 to $2!${Color_Off}"
}

benchmark_input_for_distributions() {
#    cat insert_statements.txt | cat > buffer_file.txt
#    cat $1 | cat >> buffer_file.txt

#    IFS=' '
#    read -ra LineCountInfo <<< "$(wc -l insert_statements.txt)"
#    insertLimit=${LineCountInfo[0]}
#    insertLimit=$(bc -l <<<"${insertLimit}*2")
#    insertLimit=$(bc -l <<<"${insertLimit}+1")

    benchmark_input $(echo "./benchmarkStatements/$1_23910821_23927983.txt") $2 $3 $4 "0.5" 23910821 23927983

#    rm buffer_file.txt
}

OUTPUT_FILE=$(echo "../benchmarkResults/results_${COMMIT_ID}.csv")
rm $OUTPUT_FILE
echo "Type;Dist;LoadDuration;PageSize;UserSize;ParsingTime;AnalysingTime;TranslationTime;CompilationTime;ExecutionTime;Time;TimeSec;Cycles;Instructions;L1Misses;LLCMisses;BranchMisses;TaskClock;Scale;IPC;CPUS;GHZ" | cat > $OUTPUT_FILE

mkdir "benchmarkStatements"
generate_statements 23910821 23927983

echo ""
echo "Benchmark Select Statements..."
benchmark_input_for_distributions ms_statements $OUTPUT_FILE 1 3
echo "Benchmark Select Statements with branching..."
benchmark_input_for_distributions b1s_statements $OUTPUT_FILE 2 3
benchmark_input_for_distributions b2s_statements $OUTPUT_FILE 3 3
echo "Benchmark Merge Statements..."
benchmark_input_for_distributions mm_statements $OUTPUT_FILE 4 1
echo "Benchmark Merge Statements with branching..."
benchmark_input_for_distributions b1m_statements $OUTPUT_FILE 7 1
benchmark_input_for_distributions b2m_statements $OUTPUT_FILE 8 1
echo "Benchmark Update Statements..."
benchmark_input_for_distributions mu_statements $OUTPUT_FILE 9 3
echo "Benchmark Update Statements with branching..."
benchmark_input_for_distributions b1u_statements $OUTPUT_FILE 10 3
benchmark_input_for_distributions b2u_statements $OUTPUT_FILE 11 3
echo "Benchmark Insert Statements..."
benchmark_input "./benchmarkStatements/mi_statements_30227_30303.txt" $OUTPUT_FILE 12 1 "0.5" 30227 30303
echo "Benchmark Insert Statements with branching..."
benchmark_input "./benchmarkStatements/bi_statements_30227_30303.txt" $OUTPUT_FILE 13 1 "0.5" 30227 30303
echo "Benchmark Delete Statements..."
benchmark_input_for_distributions md_statements $OUTPUT_FILE 14 1
echo "Benchmark Delete Statements with branching..."
benchmark_input_for_distributions b1d_statements $OUTPUT_FILE 15 1
benchmark_input_for_distributions b2d_statements $OUTPUT_FILE 16 1
