#!/bin/bash

computeInputStatement() {
    pageInsertFile="page_inserts.txt"
    contentInsertFile="content_inserts.txt"
    pageUpdateFile="page_updates.txt"
    contentUpdateFile="content_updates.txt"
    insertStatementsFile="insert_statements.txt"

    rm $pageInsertFile
    rm $contentInsertFile
    rm $pageUpdateFile
    rm $contentUpdateFile

    revisionFile="revision_trunc.tbl"
    pageFile="page_trunc.tbl"
    contentFile="content_trunc.tbl"

    input=$revisionFile
    currentPageId=""
    lineCounter=1
    lastLine=""
    while IFS= read -r line
    do
        IFS='|'
        read -ra VALUES <<< "$line"
        if [ "${currentPageId}" = "" ]; then
            pageline="$(grep "${VALUES[2]}|" "${pageFile}" | head -1 )"
            read -ra PAGEVALUES <<< "$pageline"
            echo "INSERT INTO page ( id , title , textId ) VALUES ( ${PAGEVALUES[0]} , '$(echo "${PAGEVALUES[1]}" | tr ' ' '_' | tr ',' '_' | tr '(' '_' | tr ')' '_' | tr '*' '_' )' , ${VALUES[3]} );" | cat >> $pageInsertFile

            contentline="$(sed "${lineCounter}!d" "${contentFile}" )"
            read -ra CONTENTVALUES <<< "$contentline"
            echo "INSERT INTO content ( id , text ) VALUES ( ${CONTENTVALUES[0]} , '$(echo "${CONTENTVALUES[1]}"  | tr ' ' '_' | tr ',' '_' | tr '(' '_' | tr ')' '_' | tr '*' '_' )' );" | cat >> $contentInsertFile

            currentPageId="${VALUES[2]}"
            ((lineCounter=lineCounter+1))
            continue
        fi
        if [ "${currentPageId}" != "${VALUES[2]}" ]; then
            if [ "${lastLine}" != "" ]; then
                read -ra UPDATEVALUES <<< "$line"
                lastLine=0
                ((lastLine=lineCounter-1))
                contentline="$(sed "${lastLine}!d" "${contentFile}" )"
                read -ra CONTENTVALUES <<< "$contentline"
                echo "INSERT INTO content VERSION branch1 ( id , text ) VALUES ( ${CONTENTVALUES[0]} , '$(echo "${CONTENTVALUES[1]}"  | tr ' ' '_' | tr ',' '_' | tr '(' '_' | tr ')' '_' | tr '*' '_' )' );" | cat >> $contentUpdateFile
                echo "UPDATE page VERSION branch1 SET textId = ${UPDATEVALUES[3]} WHERE id = ${UPDATEVALUES[2]};" | cat >> $pageUpdateFile
            fi
            pageline="$(grep "${VALUES[2]}|" "${pageFile}" | head -1 )"
            read -ra PAGEVALUES <<< "$pageline"
            echo "INSERT INTO page ( id , title , textId ) VALUES ( ${PAGEVALUES[0]} , '$(echo "${PAGEVALUES[1]}" | tr ' ' '_' | tr ',' '_' | tr '(' '_' | tr ')' '_' | tr '*' '_' )' , ${VALUES[3]} );" | cat >> $pageInsertFile

            contentline="$(sed "${lineCounter}!d" "${contentFile}" )"
            read -ra CONTENTVALUES <<< "$contentline"
            echo "INSERT INTO content ( id , text ) VALUES ( ${CONTENTVALUES[0]} , '$(echo "${CONTENTVALUES[1]}"  | tr ' ' '_' | tr ',' '_' | tr '(' '_' | tr ')' '_' | tr '*' '_' )' );" | cat >> $contentInsertFile

            currentPageId="${VALUES[2]}"
            ((lineCounter=lineCounter+1))
            continue
        fi
        lastLine="$line"
        ((lineCounter=lineCounter+1))
    done < "$input"

    cat $contentInsertFile | cat > $insertStatementsFile
    cat $pageInsertFile | cat >> $insertStatementsFile
    echo "create branch branch1 from master;" | cat >> $insertStatementsFile
    cat $contentUpdateFile | cat >> $insertStatementsFile
    cat $pageUpdateFile | cat >> $insertStatementsFile
}

computeInputStatement
