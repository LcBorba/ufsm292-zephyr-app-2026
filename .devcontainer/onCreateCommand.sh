#!/bin/bash
set -e

# west nao vem pre-instalado na imagem "ci" — so o SDK/toolchain vem.
# Instala west para o usuario atual e garante que fique no PATH.
export PATH="$HOME/.local/bin:$PATH"
echo 'export PATH="$HOME/.local/bin:$PATH"' >> "$HOME/.bashrc"
pip3 install --user west

cd /workspace

# Trata o repositório montado (que já contém west.yml na raiz) como o
# repositório-manifesto local do workspace west.
west init -l ufsm292-zephyr-app-2026

# Baixa o kernel Zephyr e os módulos (HALs SAM0, CMSIS, etc.) declarados
# no west.yml como pastas irmãs em /workspace (zephyr/, modules/, ...).
west update

west zephyr-export

pip3 install --user -r zephyr/scripts/requirements.txt

# Conveniências de terminal
echo "alias ll='ls -lah'" >> "$HOME/.bashrc"
echo "cd /workspace/ufsm292-zephyr-app-2026" >> "$HOME/.bashrc"
west completion bash > "$HOME/west-completion.bash"
echo 'source $HOME/west-completion.bash' >> "$HOME/.bashrc"

history -c
