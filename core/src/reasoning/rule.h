// reasoning/rule.h
#ifndef RULE_H
#define RULE_H

typedef struct {
    Pattern condition;
    Pattern conclusion;
    float confidence;
} Rule;
// тогда
// deduction
// становится
// Pattern found
// ↓
// instantiate variables
// ↓
// emit hypothesis

#endif
// Потому что потом появятся правила.
// Например
// IF
// A -> B
// B -> C
// THEN
// A -> C
// или
// IF
// HTTP
// +
// SQL
// ↓
// SQL Injection
// И reasoner будет выполнять именно правила.
