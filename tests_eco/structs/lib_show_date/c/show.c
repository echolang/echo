#include <stdint.h>
#include <stdio.h>

struct Date
{
    uint16_t year;
    uint8_t month;
    uint8_t day;
};

void eco_show_date(struct Date d)
{
    printf("%u-%u-%u\n", (unsigned)d.year, (unsigned)d.month, (unsigned)d.day);
}
