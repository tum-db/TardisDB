-- workspace with a column for the scores
CREATE BRANCH x1 FROM main;
ALTER TABLE t VERSION x1 ADD COLUMN score INTEGER DEFAULT 0;
-- round 2 from x2
CREATE BRANCH x10 FROM x2;
UPDATE t VERSION x10 SET b = 405, score = 54 WHERE g = 239;
CREATE BRANCH x11 FROM x2;
UPDATE t VERSION x11 SET b = 836, score = 89 WHERE g = 735;
-- ... 6 more candidates
SELECT branch, sum(b) FROM t
  VERSION (x10, x11, x12, x13, x14, x15, x16, x17)
  GROUP BY branch;
DROP BRANCH x10;
-- ... 6 more drops; rounds 3 to 50
-- resolve: keep x396
MERGE BRANCH x396 INTO main;
DROP BRANCH x1 CASCADE;
