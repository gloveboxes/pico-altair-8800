/* ============================================================
 * ENV - Environment Variable Manager for CP/M
 * ============================================================
 * Usage:
 *   ENV              - List all variables
 *   ENV NAME         - Show value of NAME
 *   ENV NAME=VALUE   - Set NAME to VALUE
 *   ENV NAME +N      - Add N to numeric NAME
 *   ENV NAME -N      - Subtract N from numeric NAME
 *   ENV -D NAME      - Delete NAME
 *   ENV -C           - Clear all variables
 *   ENV -N           - Show count of variables
 *   ENV -I NAME=VAL  - Set only if NAME not defined
 *   ENV -H           - Show help
 * ============================================================
 */

#include "stdio.h"

/* DXENV library constants (must match DXENV.C) */
#define E_KEYSZ  16
#define E_VALSZ  111
#define E_OK     0
#define E_ENOTF  -5

/* DXENV library function declarations */
int e_init();
int e_get();
int e_set();
int e_del();
int e_list();
int e_count();
int e_clear();
int e_exists();

/* LONG library function declarations (32-bit math) */
char *atol();   /* char *atol(result, s) - ascii to long */
char *ltoa();   /* char *ltoa(result, op1) - long to ascii */
char *ladd();   /* char *ladd(result, op1, op2) - add */
char *itol();   /* char *itol(result, n) - int to long */

/* Forward declarations */
int prntvar();
int parsarg();
int shwhelp();
int isnum();

/* ------------------------------------------------------------
 * isnum - Check if string is a number (with optional +/- prefix)
 * Returns 1 if numeric, 0 otherwise
 * ------------------------------------------------------------ */
int isnum(s)
char *s;
{
    int i;
    i = 0;
    if (s[i] == '+' || s[i] == '-')
        i++;
    if (s[i] == 0)
        return 0;  /* Just sign, no digits */
    while (s[i]) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        i++;
    }
    return 1;
}

/* ------------------------------------------------------------
 * shwhelp - Show usage help
 * ------------------------------------------------------------ */
int shwhelp()
{
    printf("ENV - Environment Variable Manager\r\n");
    printf("==================================\r\n");
    printf("Usage:\r\n");
    printf("  ENV           List all variables\r\n");
    printf("  ENV NAME      Show value of NAME\r\n");
    printf("  ENV NAME=VAL  Set NAME to VAL\r\n");
    printf("  ENV NAME +N   Add N to NAME\r\n");
    printf("  ENV NAME -N   Subtract N from NAME\r\n");
    printf("  ENV -D NAME   Delete NAME\r\n");
    printf("  ENV -C        Clear all variables\r\n");
    printf("  ENV -N        Show count\r\n");
    printf("  ENV -I N=V    Set if not defined\r\n");
    printf("  ENV -H        Show this help\r\n");
    printf("\r\n");
    printf("File: A:ALTAIR.ENV\r\n");
    printf("Key: max 15, Value: max 110 chars\r\n");
    return 0;
}

/* ------------------------------------------------------------
 * prntvar - Callback to print one variable
 * ------------------------------------------------------------ */
int prntvar(key, val)
char *key;
char *val;
{
    printf("%s=%s\r\n", key, val);
    return 0;
}

/* ------------------------------------------------------------
 * parsarg - Parse NAME=VALUE argument
 * Returns 1 if '=' found, 0 otherwise
 * ------------------------------------------------------------ */
int parsarg(arg, key, val)
char *arg;
char *key;
char *val;
{
    int i, j;
    
    /* Find '=' separator */
    for (i = 0; arg[i] && arg[i] != '='; i++)
        ;
    
    if (arg[i] != '=') {
        /* No '=' found - just copy key */
        j = 0;
        while (arg[j] && j < E_KEYSZ - 1) {
            key[j] = arg[j];
            j++;
        }
        key[j] = 0;
        val[0] = 0;
        return 0;
    }
    
    /* Copy key (before '=') */
    for (j = 0; j < i && j < E_KEYSZ - 1; j++)
        key[j] = arg[j];
    key[j] = 0;
    
    /* Copy value (after '=') */
    i++;  /* Skip '=' */
    j = 0;
    while (arg[i] && j < E_VALSZ - 1) {
        val[j] = arg[i];
        i++;
        j++;
    }
    val[j] = 0;
    
    return 1;
}

/* ------------------------------------------------------------
 * main - Entry point
 * ------------------------------------------------------------ */
main(argc, argv)
int argc;
char *argv[];
{
    int rc, cnt, i, j, k;
    char lcur[4], ldelta[4], lres[4];  /* 32-bit long integers */
    char lkey[E_KEYSZ];   /* Local key buffer */
    char lval[E_VALSZ];   /* Local val buffer */
    
    /* Initialize environment file */
    rc = e_init();
    if (rc != E_OK) {
        printf("Error: Cannot init env file\r\n");
        return 1;
    }
    
    /* No args - list all variables */
    if (argc < 2) {
        cnt = e_list(prntvar);
        if (cnt == 0)
            printf("(no variables set)\r\n");
        return 0;
    }
    
    /* Check for help flag */
    if (argv[1][0] == '-' && 
        (argv[1][1] == 'h' || argv[1][1] == 'H' ||
         argv[1][1] == '?')) {
        shwhelp();
        return 0;
    }
    
    /* Check for clear flag */
    if (argv[1][0] == '-' && 
        (argv[1][1] == 'c' || argv[1][1] == 'C')) {
        rc = e_clear();
        if (rc == E_OK)
            printf("All variables cleared\r\n");
        else
            printf("Error clearing variables\r\n");
        return 0;
    }
    
    /* Check for count flag */
    if (argv[1][0] == '-' && 
        (argv[1][1] == 'n' || argv[1][1] == 'N')) {
        cnt = e_count();
        printf("%d variable(s) set\r\n", cnt);
        return 0;
    }
    
    /* Check for init flag (set if not exists) */
    if (argv[1][0] == '-' && 
        (argv[1][1] == 'i' || argv[1][1] == 'I')) {
        if (argc < 3) {
            printf("Usage: ENV -I NAME=VAL\r\n");
            return 1;
        }
        
        /* Parse NAME=VALUE */
        if (!parsarg(argv[2], lkey, lval)) {
            printf("Usage: ENV -I NAME=VAL\r\n");
            return 1;
        }
        
        /* Only set if not already defined */
        if (e_exists(lkey)) {
            printf("%s already defined\r\n", lkey);
            return 0;
        }
        
        rc = e_set(lkey, lval);
        if (rc == E_OK)
            printf("%s=%s\r\n", lkey, lval);
        else
            printf("Error setting %s\r\n", lkey);
        return 0;
    }
    
    /* Check for delete flag */
    if (argv[1][0] == '-' && 
        (argv[1][1] == 'd' || argv[1][1] == 'D')) {
        if (argc < 3) {
            printf("Usage: ENV -D NAME\r\n");
            return 1;
        }
        
        /* Delete variable */
        rc = e_del(argv[2]);
        if (rc == E_OK)
            printf("%s deleted\r\n", argv[2]);
        else if (rc == E_ENOTF)
            printf("%s not found\r\n", argv[2]);
        else
            printf("Error deleting %s\r\n", argv[2]);
        return 0;
    }
    
    /* Check for increment/decrement: ENV NAME +N or ENV NAME -N */
    if (argc >= 3 && isnum(argv[2])) {
        char *deltastr;
        
        /* Get current value */
        rc = e_get(argv[1], lval);
        if (rc == E_OK) {
            /* Check if current value is numeric */
            if (!isnum(lval)) {
                printf("Error: %s is not numeric\r\n", argv[1]);
                return 1;
            }
            atol(lcur, lval);  /* Convert to 32-bit long */
        } else {
            itol(lcur, 0);  /* Start from 0 if not exists */
        }
        
        /* Apply delta using 32-bit math */
        /* Skip '+' sign since atol only handles '-' */
        deltastr = argv[2];
        if (*deltastr == '+')
            deltastr++;
        atol(ldelta, deltastr);
        ladd(lres, lcur, ldelta);
        
        /* Convert back to string and save */
        ltoa(lval, lres);
        rc = e_set(argv[1], lval);
        
        if (rc == E_OK)
            printf("%s=%s\r\n", argv[1], lval);
        else
            printf("Error setting %s\r\n", argv[1]);
        return 0;
    }
    
    /* Parse argument for NAME=VALUE */
    if (parsarg(argv[1], lkey, lval)) {
        /* Concatenate remaining args (for values with spaces) */
        k = 0;
        while (lval[k]) k++;  /* Find end of lval */
        
        for (i = 2; i < argc && k < E_VALSZ - 2; i++) {
            lval[k++] = ' ';  /* Add space separator */
            for (j = 0; argv[i][j] && k < E_VALSZ - 1; j++)
                lval[k++] = argv[i][j];
        }
        lval[k] = 0;
        
        /* Set variable */
        rc = e_set(lkey, lval);
        
        if (rc == E_OK)
            printf("%s=%s\r\n", lkey, lval);
        else
            printf("Error setting %s\r\n", lkey);
        return 0;
    }
    
    /* Just NAME - show value */
    rc = e_get(argv[1], lval);
    if (rc == E_OK)
        printf("%s=%s\r\n", argv[1], lval);
    else
        printf("%s not found\r\n", argv[1]);
    
    return 0;
}