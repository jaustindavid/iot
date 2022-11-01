#include <stdlib.h>
#include <stdio.h>
#include <string.h>


#define DEBUG_PRINTF Serial.printf
#define DEBUG_PRINTF printf

// A safe tokenizer that (a) works, (b) doesn't malloc, and 
// (c) won't overrun a static token buffer.
// if data != NULL, searches for the next separator
//   and copies data into token, max n chars, up to end or \0.
// if data == NULL, resumes search at previous separator.
// returns  if no more separators.
/*
    char token[64];
    char sep[6] = "sep'n";
    char end[2] = "\n";
    tokenize(data, sep, end, token, 64);
    while(strlen(token) > 0) {
        DEBUG_PRINTF("Token: >%s<\n", token);
        tokenize(NULL, sep, end, token, 64);
    }
 */
#undef DEBUG_TOKENIZE
char * tokenize(char *data, const char *start, const char *end,
             char *token, int n) {
    static char *placeholder = NULL;
    #ifdef DEBUG_TOKENIZE
        DEBUG_PRINTF("starting tokenize\n");
    #endif
    if (data != NULL) {
        #ifdef DEBUG_TOKENIZE
            DEBUG_PRINTF("Tokenizing %s by >%s< to next >%s<\n", 
			data ? data : "NULL", start, end);
        #endif
	placeholder = data;
    }
    placeholder = strstr(placeholder, start);
    if (data != NULL && placeholder == data) {
        DEBUG_PRINTF("trivial case: first is a token\n");
    } 
    if (placeholder) {
        placeholder += strlen(start);
        #ifdef DEBUG_TOKENIZE
            DEBUG_PRINTF("I think the next starts at >%s<\n", placeholder);
        #endif
        char *next = strstr(placeholder, end);
        if (next == NULL) {
            next = strchr(placeholder, '\0');
        }
        int len = next - placeholder + strlen(end);
        if (len > n) {
            len = n;
        }
        strncpy(token, placeholder, len);
        token[len] = '\0';
    } else {
        #ifdef DEBUG_TOKENIZE
            DEBUG_PRINTF("all done\n");
        #endif
        token[0] = '\0';
    }
    return placeholder;
} // char * tokenize(char *data, const char *start, const char *end, 
  //                 char *token, int len)


void main(void) {
	printf("hw\n");
	int buffer_len = 10240;

	char *buffer = (char *)malloc(buffer_len);
	(char *)memset(buffer, 0, buffer_len);

	FILE *f;

	f = fopen("data.txt", "r");
	fgets(buffer, buffer_len, f);
	printf("got %d bytes\n", strlen(buffer));
	printf("%s", buffer);

	char token[64];
	printf("tokenizing\n");
	tokenize(buffer, "\"dt\":", ",", token, 64);
	while (strlen(token) > 0) {
		DEBUG_PRINTF("dt: %s\n", token);
		tokenize(NULL, "\"feels_like\":", ",", token, 64);
		DEBUG_PRINTF("feels like: %s\n", token);
		tokenize(NULL, "\"dt\":", ",", token, 64);
	}
}
