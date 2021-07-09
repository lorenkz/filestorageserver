#ifndef STR2NUM_H
#define STR2NUM_H

/**
 * Convert a string into a long number and store it at the address pointed by 'n'
 *
 * Return 0 on success, 1 if not a number, 2 if an overflow/underflow occurred
 */
int str2num(const char* s, long* n);

#endif
