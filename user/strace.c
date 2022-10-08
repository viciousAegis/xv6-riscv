#include "../kernel/types.h"
#include "user.h"

int main(int argc, char *argv[]){
	if(argc <= 4){
	  fprintf(2, "usage: strace mask command [args ...]\n");
  	  exit(1);
	}
	int mask_bit = atoi(argv[1]);

	char *cmd = argv[2];
	trace(mask_bit);


	exec(cmd, &argv[2]);
	exit(0);
}