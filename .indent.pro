/* Profile file for GNU Indent for the Quantum Leaps Coding Standard */

/* 3.1 Expressions */
--no-space-after-function-call-names
--space-after-cast

/* 3.2 Indentation */
--no-tabs
--tab-size 4
--braces-after-struct-decl-line
--braces-after-if-line
--brace-indent 0
--dont-cuddle-else
--cuddle-do-while	/* inconsistent */
--case-indentation 4
--case-brace-indentation 0
--space-after-for
--space-after-if
--space-after-cast
--space-after-while
--dont-break-procedure-type
--indent-level 4
--continuation-indentation 4
--continue-at-parentheses
--line-length 90
--break-before-boolean-operator
--honour-newlines
--space-after-cast	/* --no-space-after-casts */
--blank-before-sizeof
/* -brf  */   /* opening fn brace after decl (2.2.9+) */

/* Comments */
--format-all-comments
--format-first-column-comments
--start-left-side-of-comments
--comment-line-length 80

/* Other */
--blank-lines-after-procedures	/* visually separate */

/* typedefs */
-T bool
-T uint8_t
-T uint16_t
-T uint32_t
-T int8_t
-T int16_t
-T int32_t

-T QState
-T QSTATE
-T QEvent
-T QEVENT
-T QSignal
-T QSIGNAL
-T QActive

/* Misc */
--preserve-mtime
