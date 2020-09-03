#!/bin/bash

<<STATEMENTS
MS = SELECT id FROM revision r WHERE r.pageId = <pageid>;
BS = SELECT id FROM revision VERSION branch1 r WHERE r.pageId = <pageid>;
MM = SELECT title , text FROM page p , revision r , content c WHERE p.id = r.pageId AND r.textId = c.id AND r.pageId = <pageid>;
BM = SELECT title , text FROM page p , revision VERSION branch1 r , content VERSION branch1 c WHERE r.textId = c.id AND p.id = r.pageId AND r.pageId = <pageid>;
MU = UPDATE revision SET parentid = 1 WHERE r.pageId = <pageid>;
BU = UPDATE revision VERSION branch1 SET parentid = 1 WHERE r.pageId = <pageid>;
MI = INSERT INTO content ( id , text ) VALUES ( <textid> , 'Hello_world!' );
BI = INSERT INTO content VERSION branch1 ( id , text ) VALUES (<textid> , 'Hello_World!');
MD = DELETE FROM revision WHERE pageId = <pageId>;
BD = DELETE FROM revision VERSION branch1 WHERE pageId = <pageId>;
STATEMENTS

(./semanticalBench << 'EOF'
SELECT id FROM revision r WHERE r.pageId = 10;
SELECT id FROM revision VERSION branch1 r WHERE r.pageId = 30302;
SELECT title , text FROM page p , revision r , content c WHERE p.id = r.pageId AND r.textId = c.id AND r.pageId = 10;
SELECT title , text FROM page p , revision VERSION branch1 r , content VERSION branch1 c WHERE r.textId = c.id AND r.pageId = 30302 AND p.id = r.pageId;
quit
EOF
) | cat > output.txt

input="output.txt"
while IFS= read -r line
do
  echo "$line"
done < "$input"