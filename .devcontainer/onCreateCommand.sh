#!/bin/bash
set -e

# Nesta imagem (embeddedcontainers/zephyr), west roda dentro do virtualenv
# do proprio Zephyr SDK e ja deve estar disponivel. So instala se faltar,
# sem a flag --user (que nao funciona dentro de virtualenvs).
command -v west >/dev/null 2>&1 || pip3 install west

cd /workspace

# Trata o repositório montado (que já contém west.yml na raiz) como o
# repositório-manifesto local do workspace west.
west init -l ufsm292-zephyr-app-2026

# Baixa o kernel Zephyr e os módulos (HALs, CMSIS, etc.) declarados
# no west.yml como pastas irmãs em /workspace (zephyr/, modules/, ...).
west update

# Precisa vir ANTES de "west zephyr-export": o export usa jsonschema,
# que so existe depois de instalar os requirements do Zephyr.
pip3 install -r zephyr/scripts/requirements.txt

west zephyr-export

# Conveniências de terminal
echo "alias ll='ls -lah'" >> "$HOME/.bashrc"
echo "cd /workspace/ufsm292-zephyr-app-2026" >> "$HOME/.bashrc"
west completion bash > "$HOME/west-completion.bash"
echo 'source $HOME/west-completion.bash' >> "$HOME/.bashrc"

history -c
