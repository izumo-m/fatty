#ifndef PRINT_H
#define PRINT_H

uint printer_start_enum(void);
wstring printer_get_name(uint);
void printer_finish_enum(void);

void printer_start_job(wstring printer_name);
void printer_write(char *, uint len);
void printer_finish_job(void);

#endif
