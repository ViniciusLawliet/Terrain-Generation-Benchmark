Requisitos:

CPU/OpenMP:
  GCC + Make
  RAPL/powercap → já vem no kernel

GPU/CUDA:
  NVIDIA Driver → fornece libnvidia-ml.so + nvidia-smi
  CUDA Toolkit → necessário para compilar as aplicações CUDA

Benchmark:
  GCC + Make
  headers do NVML (normalmente instalados junto com CUDA Toolkit)

------------------------------------------------------------------------

O usuário precisa ter acesso ao grupo powercap para realizar as medições com RAPL.
Segue o que funcionou para mim:

	sudo groupadd powercap
	sudo usermod -aG powercap $USER
	sudo nano /etc/udev/rules.d/70-intel-rapl.rules
	
	colar:
		ACTION=="add", SUBSYSTEM=="powercap", KERNEL=="intel-rapl:*", \
		    RUN+="/usr/bin/chgrp powercap /sys/%p/energy_uj", \
		    RUN+="/usr/bin/chmod g+r /sys/%p/energy_uj"
	
	sudo udevadm control --reload-rules
	sudo udevadm trigger --subsystem-match=powercap
	
	newgrp powercap
	

Toda vez que vou abrir um novo terminal para testes preciso iniciar com:
	newgrp powercap

Verifique a saida (rapl disponivel?):
	ls /sys/class/powercap/

-----------------------------------------------------------------------

Testes feitos no Linux: Ubuntu 22.04.5 LTS (Jammy Jellyfish).

-----------------------------------------------------------------------
Sequencia:

make 
    (dentro da pasta raiz do projeto, ele ja vai chamar os makes internos)

./benchmark/bench -b ./openmp/terrain_gen_openmp -n 10 -o ./results/sequential -- -x 2048 -z 2048 -y 256 -t 1
    (Sequencial)

./benchmark/bench -b ./openmp/terrain_gen_openmp -n 10 -o ./results/openmp_4t -- -x 2048 -z 2048 -y 256 -t 4
    (4 Threads)

./benchmark/bench -b ./cuda/terrain_gen_cuda -n 10 -g -o ./results/cuda -- -x 2048 -z 2048 -y 256
    (CUDA)

OBS: se quiser gerar os arquivos dos terrenos não precisa fazer pelo bench, basta chamar diretamente os executaveis em suas pastas e passar os parametros internos.