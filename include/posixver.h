#ifndef POSIXVER_H
#define POSIXVER_H

/**
 * This header must be included before any #include of a system header file.
 * It is best to make it the very first thing in the file, preceded only by comments.
 *
 * It is used to control the set of features available when compiling a source file.
 *
 * Defining _DEFAULT_SOURCE enables features from the 2008 edition of POSIX,
 * as well as certain BSD and SVID features without a separate feature test macro to control them.
 */

#define _DEFAULT_SOURCE

#endif
