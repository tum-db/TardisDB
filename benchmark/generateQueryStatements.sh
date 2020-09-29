<<STATEMENTS
1: MS = SELECT content FROM page p WHERE p.id = <pageid>;
2: B1S = SELECT content FROM page VERSION branch1 p WHERE p.id = <pageid>;
3: B2S = SELECT content FROM page VERSION branch2 p WHERE p.id = <pageid>;
4: MM = SELECT name FROM user u , page p WHERE p.userId = u.id AND p.id = <pageid>;
5: B1M = SELECT name FROM user u , page VERSION branch1 p WHERE p.userId = u.id AND p.id = <pageid>;
6: B2M = SELECT name FROM user u , page VERSION branch2 p WHERE p.userId = u.id AND p.id = <pageid>;
7: MU = UPDATE page SET content = 'Hello_World!' WHERE id = <pageid>;
8: B1U = UPDATE page VERSION branch1 SET content = 'Hello_World!' WHERE id = <pageid>;
9: B2U = UPDATE page VERSION branch2 SET content = 'Hello_World!' WHERE id = <pageid>;
10: MI = INSERT INTO content ( id , text ) VALUES ( <textid> , 'Hello_world!' );
11: BI = INSERT INTO content VERSION branch1 ( id , text ) VALUES (<textid> , 'Hello_World!');
12: MD = DELETE FROM page WHERE id = <pageId>;
13: B1D = DELETE FROM page VERSION branch1 WHERE id = <pageId>;
14: B2D = DELETE FROM page VERSION branch2 WHERE id = <pageId>;
STATEMENTS

generate_MS() {
    statementFile=$(echo "./benchmarkStatements/ms_statements_$1_$2.txt")
    unversionizedStatementFile=$(echo "./benchmarkStatements/ms_statements_unv_$1_$2.txt")
    rm $statementFile
    rm $unversionizedStatementFile
    for i in {1..100}
    do
        randomNumber=$(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));
        echo "SELECT content FROM page p WHERE p.id = $randomNumber;" | cat >> $statementFile
        echo "SELECT text FROM revision r , content c, page p WHERE c.id = r.textId AND p.id = r.pageId AND r.pageId = $randomNumber;" | cat >> $unversionizedStatementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_B1S() {
    statementFile=$(echo "./benchmarkStatements/b1s_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..100}
    do
        echo "SELECT content FROM page VERSION branch1 p WHERE p.id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_B2S() {
    statementFile=$(echo "./benchmarkStatements/b2s_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..100}
    do
        echo "SELECT content FROM page VERSION branch2 p WHERE p.id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_MM() {
    statementFile=$(echo "./benchmarkStatements/mm_statements_$1_$2.txt")
    unversionizedStatementFile=$(echo "./benchmarkStatements/mm_statements_unv_$1_$2.txt")
    rm $statementFile
    rm $unversionizedStatementFile
    for i in {1..100}
    do
        randomNumber=$(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));
        echo "SELECT name FROM user u , page p WHERE p.userId = u.id AND p.id = $randomNumber;" | cat >> $statementFile
        echo "SELECT name FROM revision r , content c, page p, user u WHERE u.id = r.userId AND c.id = r.textId AND p.id = r.pageId AND r.pageId = $randomNumber;" | cat >> $unversionizedStatementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_B1M() {
    statementFile=$(echo "./benchmarkStatements/b1m_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..100}
    do
        echo "SELECT name FROM user u , page VERSION branch1 p WHERE p.userId = u.id AND p.id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_B2M() {
    statementFile=$(echo "./benchmarkStatements/b2m_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..100}
    do
        echo "SELECT name FROM user u , page VERSION branch2 p WHERE p.userId = u.id AND p.id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_MU() {
    statementFile=$(echo "./benchmarkStatements/mu_statements_$1_$2.txt")
    unversionizedStatementFile=$(echo "./benchmarkStatements/mu_statements_unv_$1_$2.txt")
    rm $statementFile
    rm $unversionizedStatementFile
    for i in {1..100}
    do
        randomNumber=$(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));
        echo "UPDATE page SET content = 'Hello_World!' WHERE id = $randomNumber;" | cat >> $statementFile
        echo "SELECT id FROM revision r WHERE r.pageId = $randomNumber" | cat >> $unversionizedStatementFile
        echo "INSERT INTO revision ( id , parentId , pageId , textId , userId ) VALUES ( 2 , 1 , $randomNumber , $(($randomNumber + 1)) , 1 );" | cat >> $unversionizedStatementFile
        echo "INSERT INTO content ( id , text ) VALUES ( $(($randomNumber + 1)) , 'Hello' );" | cat >> $unversionizedStatementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_B1U() {
    statementFile=$(echo "./benchmarkStatements/b1u_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..100}
    do
        echo "UPDATE page VERSION branch1 SET content = 'Hello_World!' WHERE id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_B2U() {
    statementFile=$(echo "./benchmarkStatements/b2u_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..100}
    do
        echo "UPDATE page VERSION branch2 SET content = 'Hello_World!' WHERE id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_MI() {
    statementFile=$(echo "./benchmarkStatements/mi_statements_$1_$2.txt")
    unversionizedStatementFile=$(echo "./benchmarkStatements/mi_statements_unv_$1_$2.txt")
    rm $statementFile
    rm $unversionizedStatementFile
    for i in {1..100}
    do
        randomNumber=$(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));
        echo "INSERT INTO user ( id , name ) VALUES ( $randomNumber , 'John_Doe' );" | cat >> $statementFile
        echo "INSERT INTO user ( id , name ) VALUES ( $randomNumber , 'John_Doe' );" | cat >> $unversionizedStatementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_BI() {
    statementFile=$(echo "./benchmarkStatements/bi_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..100}
    do
        echo "INSERT INTO user VERSION branch1 ( id , name ) VALUES ( $(( ( RANDOM % ($2 - $1) ) + $1 + 1 )) , 'John_Doe');" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_MD() {
    statementFile=$(echo "./benchmarkStatements/md_statements_$1_$2.txt")
    unversionizedStatementFile=$(echo "./benchmarkStatements/md_statements_unv_$1_$2.txt")
    deleteTempFile=$(echo "./delete_temp.txt")
    rm $statementFile
    rm $unversionizedStatementFile
    for i in {1..100}
    do
        randomNumber=$(( ( RANDOM % ($2 - $1) ) + $1 + 1 ));
        echo "DELETE FROM page WHERE id = $randomNumber ;" | cat >> $statementFile
        echo "SELECT textId FROM revision r , content c WHERE r.textId = c.id AND r.pageId = $randomNumber;" | cat >> $unversionizedStatementFile
        echo "DELETE FROM page WHERE id = $randomNumber;" | cat >> $unversionizedStatementFile
        echo "DELETE FROM revision WHERE pageId = $randomNumber;" | cat >> $unversionizedStatementFile
        rm $deleteTempFile
        grep -n "|$randomNumber|" "revision_$1_$2.tbl" | cat > $deleteTempFile

        while IFS= read -r line
        do
            IFS=':'
            read -ra METRICS <<< "$line"
            if [ "${#METRICS[@]}" = "2" ]; then
                echo "DELETE FROM content WHERE id = $(bc -l <<<"${METRICS[0]}-1");" | cat >> $unversionizedStatementFile
            fi
        done < "$deleteTempFile"
    done
    echo "quit" | cat >> $statementFile
}

generate_B1D() {
    statementFile=$(echo "./benchmarkStatements/b1d_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..100}
    do
        echo "DELETE FROM page VERSION branch1 WHERE id = $(( ( RANDOM % ($2 - $1) ) + $1 + 1 )) ;" | cat >> $statementFile
    done
    echo "quit" | cat >> $statementFile
}

generate_B2D() {
    statementFile=$(echo "./benchmarkStatements/b2d_statements_$1_$2.txt")
    rm $statementFile
    for i in {1..100}
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

mkdir "benchmarkStatements"
generate_statements 23910821 23927983