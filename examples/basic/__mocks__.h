/*
Mocks header file

Generated with Nala version 0.6.0 (https://github.com/eerimoq/nala)
Do not edit manually
*/

#ifndef MOCKS_H
#define MOCKS_H

#include <stdarg.h>

#include <time.h>

#ifndef _NALA_RESET_ALL_MOCKS
#define _NALA_RESET_ALL_MOCKS 1
void nala_reset_all_mocks(void);
#endif

void nala_assert_all_mocks_completed(void);

// NALA_DECLARATION time

void time_mock(time_t return_value);
void time_mock_once(time_t return_value);
void time_mock_ignore_in(time_t return_value);
void time_mock_ignore_in_once(time_t return_value);
void time_mock_set_errno(int errno_value);
void time_mock_set___timer_in(const void *buf_p, size_t size);
void time_mock_set___timer_in_pointer(time_t *__timer);
void time_mock_set___timer_out(const void *buf_p, size_t size);
void time_mock_none(void);
void time_mock_implementation(time_t (*implementation)(time_t *__timer));
void time_mock_disable(void);
void time_mock_reset(void);
void time_mock_assert_completed(void);

#endif
