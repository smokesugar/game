#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "json.h"
#include "platform.h"

enum TokenKind {
    TOKEN_EOF,
    TOKEN_ERROR = 256,
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_NULL,
    TOKEN_STRING,
    TOKEN_INT,
    TOKEN_FLOAT,
};

struct Token {
    int kind;
    char* loc;
    int len;
    int line;
};

struct Scanner {
    int line;
    char* src;
    char* p;

    TokenKind check_keyword(char* start, const char* kw, TokenKind kind) {
        size_t len = p-start;
        if (len == strlen(kw) && memcmp(start, kw, len) == 0) {
            return kind;
        }
        return TOKEN_ERROR;
    }

    TokenKind check_keywords(char* start) {
        switch (start[0]) {
            case 't':
                return check_keyword(start, "true", TOKEN_TRUE);
            case 'f':
                return check_keyword(start, "false", TOKEN_FALSE);
            case 'n':
                return check_keyword(start, "null", TOKEN_NULL);
        }

        return TOKEN_ERROR;
    }

    Token advance() {
        while (isspace(*p)) {
            if (*p == '\n') {
                ++line;
            }
            ++p;
        }

        int start_line = line;
        char* start = p++;
        int kind = (int)*start;

        switch(*start) {
            default:
                if (isdigit(*start) || *start == '-') {
                    (void)strtof(start, &p);
                    bool dot = false;
                    for (char* c = start; c != p; ++c) {
                        if (*c == '.') {
                            dot = true;
                            break;
                        }
                    }
                    kind = dot ? TOKEN_FLOAT : TOKEN_INT;
                }
                else if (isalnum(*start)) {
                    while (isalnum(*p)) {
                        ++p;
                    }

                    kind = check_keywords(start);
                }
                break;
            case '\0':
                --p;
                break;
            case '"': {
                while (*p != '"' && *p != '\0') {
                    if (*p == '\n') {
                        ++line;
                    }
                    ++p;
                }

                if (*p == '\0') {
                    char buf[512];
                    sprintf_s(buf, sizeof(buf), "Error parsing json: unterminated string on line %d.", start_line);
                    pf_msg_box(buf);
                    kind = TOKEN_ERROR;
                }
                else {
                    ++p;
                    kind = TOKEN_STRING;
                }
            }
        }

        Token tok;
        tok.kind = kind;
        tok.loc = start;
        tok.len = (int)(p-start);
        tok.line = start_line;

        return tok;
    }

    Token peek() {
        Scanner copy = *this;
        return copy.advance();
    }

    bool match(int kind, const char* what) {
        Token tok = peek();

        if (tok.kind == kind) {
            advance();
            return true;
        }

        char buf[512];
        sprintf_s(buf, sizeof(buf), "Error parsing json: expected %s on line %d.", what, tok.line);
        pf_msg_box(buf);

        return false;
    }
};

#define consume(scanner, kind, what) if (!(scanner)->match(kind, what)) { return {}; }

static char* extract_string(Arena* arena, Token tok) {
    assert(tok.kind == TOKEN_STRING);

    int len = tok.len - 2;
    char* str = arena->push_array<char>(len + 1);

    memcpy(str, tok.loc + 1, len);
    str[len] = '\0';

    return str;
}

static JSON parse(Arena* arena, Scanner* scanner) {
    Token tok = scanner->advance();

    switch (tok.kind) {
        default: {
            char buf[512];
            sprintf_s(buf, sizeof(buf), "Error parsing json: unexpected token on line %d.", tok.line);
            pf_msg_box(buf);
            return {};
        }

        case TOKEN_INT: {
            JSON json;
            json.type = JSON_INT;
            json.as._int = strtol(tok.loc, 0, 10);
            return json;
        }

        case TOKEN_FLOAT: {
            JSON json;
            json.type = JSON_FLOAT;
            json.as._float = strtof(tok.loc, 0);
            return json;
        }

        case TOKEN_NULL: {
            JSON json;
            json.type = JSON_NULL;
            return json;
        }

        case TOKEN_TRUE:
        case TOKEN_FALSE:
        {
            JSON json;
            json.type = JSON_BOOLEAN;
            json.as.boolean = tok.kind == TOKEN_TRUE;
            return json;
        }

        case TOKEN_STRING: {
            JSON json;
            json.type = JSON_STRING;
            json.as.string = extract_string(arena, tok);
            return json;
        }

        case '[': {
            Vec<JSON> list = {};
            
            while (scanner->peek().kind != ']' && scanner->peek().kind != TOKEN_EOF) {
                if (!list.empty()) {
                    consume(scanner, ',', ",");
                }

                JSON json = parse(arena, scanner);
                if (!json.type) return {};

                list.push(json);
            }

            consume(scanner, ']', "]");

            JSON json;
            json.type = JSON_ARRAY;
            json.as.array.len = list.len;
            json.as.array.mem = arena->push_array<JSON>(list.len);

            for (u32 i = 0; i < list.len; ++i) {
                json.as.array.mem[i] = list[i];
            }

            list.free();

            return json;
        }

        case '{': {
            Vec<JSONPair> list = {};

            while (scanner->peek().kind != '}' && scanner->peek().kind != TOKEN_EOF) {
                if (!list.empty()) {
                    consume(scanner, ',', ",");
                }

                Token name = scanner->peek();
                consume(scanner, TOKEN_STRING, "a string");
                consume(scanner, ':', ":");

                JSON json = parse(arena, scanner);
                if (!json.type) return {};

                JSONPair pair;
                pair.name = extract_string(arena, name);
                pair.json = json;

                list.push(pair);
            }

            consume(scanner, '}', "}");

            JSON json;
            json.type = JSON_OBJECT;
            json.as.object.count = list.len;
            json.as.object.mem = arena->push_array<JSONPair>(list.len);

            for (u32 i = 0; i < list.len; ++i) {
                json.as.object.mem[i] = list[i];
            }

            list.free();

            return json;
        }
    }
}

JSON parse_json(Arena* arena, char* str) {
    Scanner scanner;
    scanner.line = 1;
    scanner.src = str;
    scanner.p = str;

    return parse(arena, &scanner);
}
