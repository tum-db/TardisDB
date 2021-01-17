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

echo ""
echo "Benchmark Select Statements..."
benchmark_input_for_distributions ms_statements $OUTPUT_FILE 1 3
echo "Benchmark Select Statements with branching..."
benchmark_input_for_distributions b1s_statements $OUTPUT_FILE 2 3
benchmark_input_for_distributions b2s_statements $OUTPUT_FILE 3 3
echo "Benchmark Merge Statements..."
benchmark_input_for_distributions mm_statements $OUTPUT_FILE 4 1
echo "Benchmark Merge Statements with branching..."
benchmark_input_for_distributions b1m_statements $OUTPUT_FILE 5 1
benchmark_input_for_distributions b2m_statements $OUTPUT_FILE 6 1
echo "Benchmark Update Statements..."
benchmark_input_for_distributions mu_statements $OUTPUT_FILE 7 3
echo "Benchmark Update Statements with branching..."
benchmark_input_for_distributions b1u_statements $OUTPUT_FILE 8 3
benchmark_input_for_distributions b2u_statements $OUTPUT_FILE 9 3
echo "Benchmark Insert Statements..."
benchmark_input "./benchmarkStatements/mi_statements_23910821_23927983.txt" $OUTPUT_FILE 10 1 "0.5" 23910821 23927983
echo "Benchmark Insert Statements with branching..."
benchmark_input "./benchmarkStatements/bi_statements_23910821_23927983.txt" $OUTPUT_FILE 11 1 "0.5" 23910821 23927983
echo "Benchmark Delete Statements..."
benchmark_input_for_distributions md_statements $OUTPUT_FILE 12 1
echo "Benchmark Delete Statements with branching..."
benchmark_input_for_distributions b1d_statements $OUTPUT_FILE 13 1
benchmark_input_for_distributions b2d_statements $OUTPUT_FILE 14 1
