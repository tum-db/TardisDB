#!/bin/bash

benchmark_input() {
    # Execute benchmark program and write output to file
    (./semanticalBench "-d=$3" < $1) | cat > output.txt

    # Declare metric arrays
    declare -a parsing_times
    declare -a analysing_times
    declare -a translation_times
    declare -a compile_times
    declare -a execution_times
    declare -a sums

    # Retrieve metrics from file
    input="output.txt"
    while IFS= read -r line
    do
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

    # Append metrics to csv file
    echo "$2;\"$3\";${parsing_time};${analysing_time};${translation_time};${compile_time};${execution_time};${sum}" | cat >> output.csv
}

<<STATEMENTS
1: MS = SELECT id FROM revision r WHERE r.pageId = <pageid>;
2: BS = SELECT id FROM revision VERSION branch1 r WHERE r.pageId = <pageid>;
3: MM = SELECT title , text FROM page p , revision r , content c WHERE p.id = r.pageId AND r.textId = c.id AND r.pageId = <pageid>;
4: BM = SELECT title , text FROM page p , revision VERSION branch1 r , content VERSION branch1 c WHERE r.textId = c.id AND p.id = r.pageId AND r.pageId = <pageid>;
5: MU = UPDATE revision SET parentid = 1 WHERE r.pageId = <pageid>;
6: BU = UPDATE revision VERSION branch1 SET parentid = 1 WHERE r.pageId = <pageid>;
7: MI = INSERT INTO content ( id , text ) VALUES ( <textid> , 'Hello_world!' );
8: BI = INSERT INTO content VERSION branch1 ( id , text ) VALUES (<textid> , 'Hello_World!');
9: MD = DELETE FROM revision WHERE pageId = <pageId>;
10: BD = DELETE FROM revision VERSION branch1 WHERE pageId = <pageId>;
STATEMENTS

generate_MS() {
    rm ms_statements.txt
    for i in {1..50}
    do
        echo "SELECT id FROM revision r WHERE r.pageId = $(( ( RANDOM % 30303 )  + 1 ));" | cat >> ms_statements.txt
    done
    echo "quit" | cat >> ms_statements.txt
}

generate_BS() {
    rm bs_statements.txt
    for i in {1..50}
    do
        echo "SELECT id FROM revision VERSION branch1 r WHERE r.pageId = $(( ( RANDOM % 30303 )  + 1 ));" | cat >> bs_statements.txt
    done
    echo "quit" | cat >> bs_statements.txt
}

generate_MM() {
    rm mm_statements.txt
    for i in {1..50}
    do
        echo "SELECT title , text FROM page p , revision r , content c WHERE p.id = r.pageId AND r.textId = c.id AND r.pageId = $(( ( RANDOM % 30303 )  + 1 ));" | cat >> mm_statements.txt
    done
    echo "quit" | cat >> mm_statements.txt
}

generate_BM() {
    rm bm_statements.txt
    for i in {1..50}
    do
        echo "SELECT title , text FROM page p , revision VERSION branch1 r , content VERSION branch1 c WHERE r.textId = c.id AND p.id = r.pageId AND r.pageId = $(( ( RANDOM % 30303 )  + 1 ));" | cat >> bm_statements.txt
    done
    echo "quit" | cat >> bm_statements.txt
}

generate_MU() {
    rm mu_statements.txt
    for i in {1..50}
    do
        echo "UPDATE revision SET parentid = 1 WHERE pageId = $(( ( RANDOM % 30303 )  + 1 ));" | cat >> mu_statements.txt
    done
    echo "quit" | cat >> mu_statements.txt
}

generate_BU() {
    rm bu_statements.txt
    for i in {1..50}
    do
        echo "UPDATE revision VERSION branch1 SET parentid = 1 WHERE pageId = $(( ( RANDOM % 30303 )  + 1 ));" | cat >> bu_statements.txt
    done
    echo "quit" | cat >> bu_statements.txt
}

generate_MI() {
    rm mi_statements.txt
    for i in {1..50}
    do
        echo "INSERT INTO content ( id , text ) VALUES ( $(( ( RANDOM % 30303 )  + 1 )) , 'Hello_world!' );" | cat >> mi_statements.txt
    done
    echo "quit" | cat >> mi_statements.txt
}

generate_BI() {
    rm bi_statements.txt
    for i in {1..50}
    do
        echo "INSERT INTO content VERSION branch1 ( id , text ) VALUES ( $(( ( RANDOM % 30303 )  + 1 )) , 'Hello_World!');" | cat >> bi_statements.txt
    done
    echo "quit" | cat >> bi_statements.txt
}

generate_MD() {
    rm md_statements.txt
    for i in {1..50}
    do
        echo "DELETE FROM revision WHERE pageId = $(( ( RANDOM % 30303 )  + 1 )) ;" | cat >> md_statements.txt
    done
    echo "quit" | cat >> md_statements.txt
}

generate_BD() {
    rm bd_statements.txt
    for i in {1..50}
    do
        echo "DELETE FROM revision VERSION branch1 WHERE pageId = $(( ( RANDOM % 30303 )  + 1 )) ;" | cat >> bd_statements.txt
    done
    echo "quit" | cat >> bd_statements.txt
}

benchmark_input_for_distributions() {
    benchmark_input $1 $2 0.9999,0.0001
    benchmark_input $1 $2 0.999,0.001
    benchmark_input $1 $2 0.99,0.01
    benchmark_input $1 $2 0.9,0.1
    benchmark_input $1 $2 0.5,0.5
    benchmark_input $1 $2 0.1,0.9
    benchmark_input $1 $2 0.01,0.99
    benchmark_input $1 $2 0.001,0.999
    benchmark_input $1 $2 0.0001,0.9999
}

rm output.csv
echo "ParsingTime;AnalysingTime;TranslationTime;CompilationTime;ExecutionTime;Time" | cat > output.csv

generate_MS
generate_BS
echo "Generated Select Statements!"
generate_MM
generate_BM
echo "Generated Merge Statements!"
generate_MU
generate_BU
echo "Generated Update Statements!"
generate_MI
generate_BI
echo "Generated Insert Statements!"
generate_MD
generate_BD
echo "Generated Delete Statements!"

echo ""
echo "Benchmark Select Statements..."
benchmark_input_for_distributions ms_statements.txt 1
echo "Benchmark Select Statements with branching..."
benchmark_input_for_distributions bs_statements.txt 2
echo "Benchmark Merge Statements..."
benchmark_input_for_distributions mm_statements.txt 3
echo "Benchmark Merge Statements with branching..."
benchmark_input_for_distributions bm_statements.txt 4
echo "Benchmark Update Statements..."
benchmark_input_for_distributions mu_statements.txt 5
echo "Benchmark Update Statements with branching..."
benchmark_input_for_distributions bu_statements.txt 6
echo "Benchmark Insert Statements..."
benchmark_input mi_statements.txt 7 "0.5,0.5"
echo "Benchmark Insert Statements with branching..."
benchmark_input bi_statements.txt 8 "0.5,0.5"
echo "Benchmark Delete Statements..."
benchmark_input_for_distributions md_statements.txt 9
echo "Benchmark Delete Statements with branching..."
benchmark_input_for_distributions bd_statements.txt 10
