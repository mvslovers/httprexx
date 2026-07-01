         TITLE 'HRXTERM - C-callable IRXTERM wrapper for HTTPREXX'
*
*  HRXTERM - terminate a REXX/370 Language Processor Environment.
*
*  Called from C (cc370) as:
*       extern int httprexx_irxterm(void *env) asm("HRXTERM");
*
*  IRXTERM takes the ENVBLOCK in R0 (no parameter list), so it cannot
*  be reached through __linkds(), which does not control R0. This shim
*  LOADs IRXTERM at runtime (the module is installed on the system, not
*  statically linked, so a static V-con would be unresolved under NCAL)
*  and calls it with R0 = env.
*
*  Entry:  R1 -> argument list; arg0 (env) is the VALUE at 0(R1); cc370
*               passes argument values in the R1 list (single deref).
*          R13 -> caller 72-byte save area, R14 = return, R15 = entry.
*  Return: R15 = IRXTERM return code (0 ok, 20 bad env); -1 = LOAD fail.
*
*  Linkage modeled on rexx370 asm/istso.asm: manual OS linkage with a
*  GETMAIN workarea (RENT/REUS) that also serves as IRXTERM's save area.
*
*  NOTE: keep every statement within column 71. Column 72 is the
*  continuation column; a comment that reaches it silently swallows the
*  next line (this once ate the BALR that sets the base register).
*
*  (c) 2026 mvslovers
*
         PRINT NOGEN
R0       EQU   0
R1       EQU   1
R3       EQU   3
R4       EQU   4
R9       EQU   9
R12      EQU   12
R13      EQU   13
R14      EQU   14
R15      EQU   15
         PRINT GEN
*
HRXTERM  CSECT
         STM   R14,R12,12(R13)    save caller regs
         BALR  R12,0              establish base register
         USING *,R12
*
*  Read env (arg0) before GETMAIN/LOAD clobber R1.
         L     R3,0(,R1)          R3 = env
*
*  Acquire a dynamic workarea / save area (RENT).
         L     R0,=A(WALEN)
         GETMAIN RU,LV=(0)
         LR    R9,R1              R9 = workarea (for FREEMAIN)
         ST    R13,4(,R1)         back chain
         ST    R1,8(,R13)         forward chain
         LR    R13,R1
         USING WAREA,R13
*
*  LOAD IRXTERM, call it with R0 = env, then DELETE to balance.
         LOAD  EP=IRXTERM,ERRET=NOLOAD
         LR    R15,R0             R15 = IRXTERM entry
         LR    R0,R3              R0  = env
         BALR  R14,R15            call IRXTERM
         LR    R4,R15             R4  = RC (preserve)
         DELETE EP=IRXTERM
         B     EXIT
*
NOLOAD   LA    R4,1               R4 = 1
         LCR   R4,R4              R4 = -1 (LOAD failed)
*
EXIT     DS    0H
         L     R13,WDPREV         R13 = caller save area
         L     R0,=A(WALEN)
         FREEMAIN RU,LV=(0),A=(9)
         LR    R15,R4             R15 = return code
         L     R14,12(,R13)       restore return address
         LM    R0,R12,20(,R13)    restore R0-R12
         BR    R14
*
         LTORG
*
WAREA    DSECT
WDFLAGS  DS    F                  +0  reserved
WDPREV   DS    F                  +4  back chain
WDNEXT   DS    F                  +8  forward chain
         DS    15F                +12..+71 save slots
WALEN    EQU   *-WAREA
*
         END   HRXTERM
