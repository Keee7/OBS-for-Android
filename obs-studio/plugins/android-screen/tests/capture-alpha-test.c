/* Screen capture alpha conversion regression cases. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../capture-alpha.h"
int main(void)
{
 const uint8_t input[] = {0,0,0,0, 64,32,16,128, 20,40,255,255, 255,2,1,1};
 const uint8_t expected[] = {0,0,0,0, 128,64,32,128, 20,40,255,255, 255,255,255,1};
 uint8_t output[sizeof(input)] = {0};
 screen_alpha_unpremultiply(output, input, 4);
 assert(memcmp(output, expected, sizeof(input)) == 0);
 return 0;
}
