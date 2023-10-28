/*
crtlib.h - internal stdlib
Copyright (C) 2011 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#ifndef STDLIB_H
#define STDLIB_H

#include <string.h>
#include <stdarg.h>
#include "build.h"
#include "xash3d_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

// timestamp modes
enum
{
	TIME_FULL = 0,
	TIME_DATE_ONLY,
	TIME_TIME_ONLY,
	TIME_NO_SECONDS,
	TIME_YEAR_ONLY,
	TIME_FILENAME,
};

// a1ba: not using BIT macro, so flags can be copypasted into
// exported APIs headers and will get nice warning in case of changing values
#define PFILE_IGNOREBRACKET (1<<0)
#define PFILE_HANDLECOLON   (1<<1)
#define PFILE_TOKEN_MAX_LENGTH 1024
#define PFILE_FS_TOKEN_MAX_LENGTH 512

//
// build.c
//
int EXPORT Q_buildnum( void );
int EXPORT Q_buildnum_date( const char *date );
int EXPORT Q_buildnum_compat( void );
const char EXPORT *Q_PlatformStringByID( const int platform );
const char EXPORT *Q_buildos( void );
const char EXPORT *Q_ArchitectureStringByID( const int arch, const uint abi, const int endianness, const qboolean is64 );
const char EXPORT *Q_buildarch( void );
const char EXPORT *Q_buildcommit( void );

//
// crtlib.c
//
void EXPORT Q_strnlwr( const char *in, char *out, size_t size_out );
#define Q_strlen( str ) (( str ) ? strlen(( str )) : 0 )
size_t EXPORT Q_colorstr( const char *string );
char EXPORT Q_toupper( const char in );
char EXPORT Q_tolower( const char in );
size_t EXPORT Q_strncat( char *dst, const char *src, size_t siz );
qboolean EXPORT Q_isdigit( const char *str );
qboolean EXPORT Q_isspace( const char *str );
int EXPORT Q_atoi( const char *str );
float EXPORT Q_atof( const char *str );
void EXPORT Q_atov( float *vec, const char *str, size_t siz );
#define Q_strchr  strchr
#define Q_strrchr strrchr
qboolean EXPORT Q_stricmpext( const char *pattern, const char *text );
qboolean EXPORT Q_strnicmpext( const char *pattern, const char *text, size_t minimumlen );
const byte EXPORT *Q_memmem( const byte *haystack, size_t haystacklen, const byte *needle, size_t needlelen );
const char EXPORT *Q_timestamp( int format );
int EXPORT Q_vsnprintf( char *buffer, size_t buffersize, const char *format, va_list args );
int EXPORT Q_snprintf( char *buffer, size_t buffersize, const char *format, ... ) _format( 3 );
#define Q_strpbrk strpbrk
void EXPORT COM_StripColors( const char *in, char *out );
#define Q_memprint( val ) Q_pretifymem( val, 2 )
char EXPORT *Q_pretifymem( float value, int digitsafterdecimal );
void EXPORT COM_FileBase( const char *in, char *out, size_t size );
const char EXPORT *COM_FileExtension( const char *in );
void EXPORT COM_DefaultExtension( char *path, const char *extension, size_t size );
void EXPORT COM_ReplaceExtension( char *path, const char *extension, size_t size );
void EXPORT COM_ExtractFilePath( const char *path, char *dest );
const char EXPORT *COM_FileWithoutPath( const char *in );
void EXPORT COM_StripExtension( char *path );
void EXPORT COM_RemoveLineFeed( char *str );
void EXPORT COM_FixSlashes( char *pname );
void EXPORT COM_PathSlashFix( char *path );
char EXPORT COM_Hex2Char( uint8_t hex );
void EXPORT COM_Hex2String( uint8_t hex, char *str );
// return 0 on empty or null string, 1 otherwise
#define COM_CheckString( string ) ( ( !string || !*string ) ? 0 : 1 )
#define COM_CheckStringEmpty( string ) ( ( !*string ) ? 0 : 1 )
char EXPORT *COM_ParseFileSafe( char *data, char *token, const int size, unsigned int flags, int *len, qboolean *quoted );
#define COM_ParseFile( data, token, size ) COM_ParseFileSafe( data, token, size, 0, NULL, NULL )
int EXPORT matchpattern( const char *in, const char *pattern, qboolean caseinsensitive );
int EXPORT matchpattern_with_separator( const char *in, const char *pattern, qboolean caseinsensitive, const char *separators, qboolean wildcard_least_one );

// libc implementations
static inline int Q_strcmp( const char *s1, const char *s2 )
{
	return unlikely(!s1) ?
		( !s2 ? 0 : -1 ) :
		( unlikely(!s2) ? 1 : strcmp( s1, s2 ));
}

static inline int Q_strncmp( const char *s1, const char *s2, size_t n )
{
	return unlikely(!s1) ?
		( !s2 ? 0 : -1 ) :
		( unlikely(!s2) ? 1 : strncmp( s1, s2, n ));
}

static inline char *Q_strstr( const char *s1, const char *s2 )
{
	return unlikely( !s1 || !s2 ) ? NULL : (char*)strstr( s1, s2 );
}

// Q_strncpy is the same as strlcpy
static inline size_t Q_strncpy( char *dst, const char *src, size_t siz )
{
	size_t len;

	if( unlikely( !dst || !src || !siz ))
		return 0;

	len = strlen( src );
	if( len + 1 > siz ) // check if truncate
	{
		memcpy( dst, src, siz - 1 );
		dst[siz - 1] = 0;
	}
	else memcpy( dst, src, len + 1 );

	return len; // count does not include NULL
}

// libc extensions, be careful

#if XASH_WIN32
#define strcasecmp stricmp
#define strncasecmp strnicmp
#endif // XASH_WIN32

static inline int Q_stricmp( const char *s1, const char *s2 )
{
	return unlikely(!s1) ?
		( !s2 ? 0 : -1 ) :
		( unlikely(!s2) ? 1 : strcasecmp( s1, s2 ));
}

static inline int Q_strnicmp( const char *s1, const char *s2, size_t n )
{
	return unlikely(!s1) ?
		( !s2 ? 0 : -1 ) :
		( unlikely(!s2) ? 1 : strncasecmp( s1, s2, n ));
}

#if defined( HAVE_STRCASESTR )
#if XASH_WIN32
#define strcasestr stristr
#endif
static inline char *Q_stristr( const char *s1, const char *s2 )
{
	return unlikely( !s1 || !s2 ) ? NULL : (char *)strcasestr( s1, s2 );
}
#else // defined( HAVE_STRCASESTR )
char *Q_stristr( const char *s1, const char *s2 );
#endif // defined( HAVE_STRCASESTR )

#if defined( HAVE_STRCHRNUL )
#define Q_strchrnul strchrnul
#else
static inline const char *Q_strchrnul( const char *s, int c )
{
	const char *p = Q_strchr( s, c );

	if( p )
		return p;

	return s + Q_strlen( s );
}
#endif

#ifdef __cplusplus
}
#endif

#endif//STDLIB_H
