#!/bin/bash

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