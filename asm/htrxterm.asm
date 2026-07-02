         TITLE 'HRXCALL - call a routine with the ENVBLOCK in R0'
*
*  HRXCALL - call a loaded routine (IRXTERM) with R0 = ENVBLOCK.
*
*  Called from C (cc370) as:
*       extern int hrx_call(void *ep, void *env) asm("HRXCALL");
*
*  IRXTERM takes the ENVBLOCK in R0 (no parameter list), which no C
*  calling convention nor __linkds can set. The caller obtains the
*  IRXTERM entry point in C via __load() (the crent370/libc370 loader
*  that rexx370 itself uses -- the as370 LOAD macro's return-code form
*  is not honored by the MVS 3.8j SVC 8), then passes (ep, env) here.
*
*  Entry:  R1 -> arg list; arg0 (ep) at 0(R1), arg1 (env) at 4(R1);
*               cc370 passes argument values in the R1 list.
*          R13 -> caller 72-byte save area, R14 = return, R15 = entry.
*  Return: R15 = the routine's return code.
*
*  Linkage modeled on rexx370 asm/istso.asm: manual OS linkage with a
*  GETMAIN workarea that also serves as the routine's save area.
*
*  NOTE: keep every statement within column 71 -- column 72 is the
*  continuation column and would swallow the next line.
*
*  (c) 2026 mvslovers
*
         PRINT NOGEN
R0       EQU   0
R1       EQU   1
R2       EQU   2
R3       EQU   3
R4       EQU   4
R9       EQU   9
R12      EQU   12
R13      EQU   13
R14      EQU   14
R15      EQU   15
         PRINT GEN
*
HRXCALL  CSECT
         STM   R14,R12,12(R13)    save caller regs
         BALR  R12,0              establish base register
         USING *,R12
*
*  Read the arguments (values) before GETMAIN clobbers R1.
         L     R2,0(,R1)          R2 = ep  (routine entry)
         L     R3,4(,R1)          R3 = env (ENVBLOCK)
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
*  Call the routine with R0 = env.
         LR    R15,R2             R15 = ep
         LR    R0,R3              R0  = env
         BALR  R14,R15            call the routine
         LR    R4,R15             R4  = RC (preserve)
*
         L     R13,WDPREV         R13 = caller save area
         L     R0,=A(WALEN)
         FREEMAIN RU,LV=(0),A=(9)
         LR    R15,R4             R15 = return code
         L     R14,12(,R13)       restore return address
*  RS format needs D(B): as370 silently assembles the RX-style
*  D(,B) form with BASE=0, i.e. LM from PSA low core -- this was
*  the root cause of the "IRXTERM crashes from a C host" bug
*  (rexx370 docs/irxterm-c-host-crash.md).
         LM    R0,R12,20(R13)     restore R0-R12
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
         END   HRXCALL
