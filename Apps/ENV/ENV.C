/* ============================================================
 * ENV - Environment Variable Manager for CP/M
 * ============================================================
 * Usage:
 *   ENV              - List all variables
 *   ENV NAME         - Show value of NAME
 *   ENV NAME=VALUE   - Set NAME to VALUE
 *   ENV -D NAME      - Delete NAME
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

/* Forward declarations */
int prntvar();
int parsarg();

/* Global buffers */
char g_key[E_KEYSZ];
char g_val[E_VALSZ];

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
    int rc, cnt;
    
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
    
    /* Parse argument for NAME=VALUE */
    if (parsarg(argv[1], g_key, g_val)) {
        /* Set variable */
        rc = e_set(g_key, g_val);
        if (rc == E_OK)
            printf("%s=%s\r\n", g_key, g_val);
        else
            printf("Error setting %s\r\n", g_key);
        return 0;
    }
    
    /* Just NAME - show value */
    rc = e_get(argv[1], g_val);
    if (rc == E_OK)
        printf("%s=%s\r\n", argv[1], g_val);
    else
        printf("%s not found\r\n", argv[1]);
    
    return 0;
}
