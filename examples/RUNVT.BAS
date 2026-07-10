10 REM RUNVT.BAS - quick exercise for the RunVT terminal emulator
20 REM run with: MBASIC RUNVT
30 ESC$=CHR$(27)
40 CLS$=ESC$+"[2J"+ESC$+"[H"
45 WIDTH 255
46 REM disable MBASIC's own line-wrap - it counts invisible ANSI
47 REM escape bytes as consuming columns, so its wrap logic can
48 REM miscount and insert a spurious CRLF mid-sentence otherwise
50 GS$=ESC$+"(0" : NG$=ESC$+"(B"
60 PRINT CLS$;
70 PRINT "RunVT terminal test"
80 PRINT "===================="
90 PRINT
100 REM --- 8 normal colors ---
110 PRINT "Colors (normal):"
120 FOR C=30 TO 37
130   N$=MID$(STR$(C),2)
140   PRINT ESC$;"[";N$;"m";"##";
150 NEXT C
160 PRINT ESC$;"[0m"
170 REM --- 8 bright colors ---
180 PRINT "Colors (bright):"
190 FOR C=90 TO 97
200   N$=MID$(STR$(C),2)
210   PRINT ESC$;"[";N$;"m";"##";
220 NEXT C
230 PRINT ESC$;"[0m"
240 PRINT
250 REM --- attributes ---
260 PRINT ESC$;"[1m";"Bold";ESC$;"[0m ";
270 PRINT ESC$;"[4m";"Underline";ESC$;"[0m ";
280 PRINT ESC$;"[7m";"Reverse";ESC$;"[0m"
290 PRINT
300 REM --- accented chars, so you know the codepage is right ---
310 PRINT "Acentos: ";CHR$(227);CHR$(231);CHR$(245);CHR$(225);CHR$(233)
320 PRINT
330 REM --- DEC Special Graphics box drawing ---
340 PRINT "Box drawing:"
350 PRINT GS$;"l";STRING$(10,"q");"k";NG$
360 FOR R=1 TO 3
370   PRINT GS$;"x";NG$;"          ";GS$;"x";NG$
380 NEXT R
390 PRINT GS$;"m";STRING$(10,"q");"j";NG$
400 PRINT
410 REM --- sixel image inline, right after this line of text ---
420 PRINT "Sixel bars below this line (red/green/blue):"
430 PRINT ESC$;"Pq";
440 PRINT "#1;2;100;0;0#2;2;0;100;0#3;2;0;0;100";
450 PRINT "#1!60~-#2!60~-#3!60~";
460 PRINT ESC$;"\";
470 PRINT
480 PRINT "Text right after an image works fine too."
490 PRINT
500 REM --- same image, this time positioned beside text with CUP.
510 REM     Fixed row numbers on purpose: MBASIC's own CSRLIN tracking
520 REM     doesn't know ANSI escapes are zero-width, so it drifts out
530 REM     of sync with the real cursor after all the escapes above.
540 PRINT ESC$;"[20;1H";"Sixel can sit beside text too - look to the right:"
550 PRINT ESC$;"[20;50H";
560 PRINT ESC$;"Pq";
570 PRINT "#1;2;100;0;0#2;2;0;100;0#3;2;0;0;100";
580 PRINT "#1!30~-#2!30~-#3!30~";
590 PRINT ESC$;"\";
600 PRINT ESC$;"[24;1H";"This line sits below both images, clear of either one."
610 PRINT
615 INPUT "Press ENTER to continue to the scroll/erase/shift tests", A$
620 PRINT
630 REM --- sixel + scroll: the image should move with the text,
640 REM     not stay pinned to its original screen position ---
650 PRINT CLS$;
660 PRINT "SCROLL TEST: the red bar below should scroll UP along with"
670 PRINT "this text. If it stays pinned in place while everything"
680 PRINT "else moves past it, that's the bug this checks for."
690 PRINT
700 PRINT ESC$;"Pq";
710 PRINT "#1;2;100;0;0!160~-!160~-!160~";
720 PRINT ESC$;"\";
730 PRINT
740 FOR N=1 TO 30
750   PRINT "scrolling line";N
760 NEXT N
770 PRINT
780 INPUT "If the bar scrolled off with the text, press ENTER", A$
790 REM --- sixel + erase: EL/ED should punch a hole in the image,
800 REM     not just clear the text cells underneath it ---
810 PRINT CLS$;
820 PRINT "ERASE TEST: a red bar is about to be drawn on row 10. Then"
830 PRINT "we erase from partway through that row to the end - the"
840 PRINT "right half of the bar should vanish, not stay visible over"
850 PRINT "the now-blank text."
860 PRINT
870 PRINT ESC$;"[10;1H";
875 REM 2 bands (12px), not 3 - keeps the bar within one 16px text
876 REM row so the erase test doesn't show a false-looking sliver
877 REM left over from pixels that were never in row 10 to begin with
880 PRINT ESC$;"Pq";
890 PRINT "#1;2;100;0;0!160~-!160~";
900 PRINT ESC$;"\";
905 REM sixel drawing doesn't move the cursor, so without an explicit
906 REM CUP here the prompt below would land right on top of the bar
907 PRINT ESC$;"[15;1H";ESC$;"[K";
910 INPUT "Press ENTER to erase the right half", A$
920 PRINT ESC$;"[10;11H";ESC$;"[K";
925 PRINT ESC$;"[15;1H";ESC$;"[K";
930 INPUT "Right half of the bar should be gone now. Press ENTER", A$
940 REM --- sixel + insert/delete char: the image should shift with
950 REM     the text instead of staying put while text moves ---
960 PRINT CLS$;
970 PRINT "SHIFT TEST: a red bar on row 10, columns 1-20. Deleting 10"
980 PRINT "characters at the start of that row should shift the bar"
990 PRINT "left by 10 columns along with everything else."
1000 PRINT
1010 PRINT ESC$;"[10;1H";
1015 REM 2 bands again, same reason as the erase test above
1020 PRINT ESC$;"Pq";
1030 PRINT "#1;2;100;0;0!160~-!160~";
1040 PRINT ESC$;"\";
1045 PRINT ESC$;"[15;1H";ESC$;"[K";
1050 INPUT "Press ENTER to delete 10 chars at the start of that row", A$
1060 PRINT ESC$;"[10;1H";ESC$;"[10P";
1065 PRINT ESC$;"[15;1H";ESC$;"[K";
1070 INPUT "Bar should have shifted left 10 cols. Press ENTER", A$
1080 PRINT ESC$;"[10;1H";ESC$;"[10@";
1085 PRINT ESC$;"[15;1H";ESC$;"[K";
1090 INPUT "Inserted 10 blanks back - bar should shift right again. ENTER", A$
1100 PRINT CLS$;
1110 INPUT "All done - press ENTER to finish", A$
1120 PRINT ESC$;"[0m";
1130 END
