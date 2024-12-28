alias ncdu='ncdu --color dark'
alias vim=nvim

alias tmux-hello-lua='cd $HOME && \
    tmux new-session -s lua \;\
        split-window -v -p 30 \;\
        send-keys -t lua:0.0 "nvim hello.lua" C-m \;\
        send-keys -t lua:0.1 "lua hello.lua" C-m'

alias tmux-hello-js='cd $HOME && \
tmux new-session -s js \;\
        split-window -v -p 30 \;\
        send-keys -t js:0.0 "nvim hello.js" C-m \;\
        send-keys -t js:0.1 "qjs hello.js" C-m'

alias tmux-hello-py='cd $HOME && \
tmux new-session -s py \;\
        split-window -v -p 30 \;\
        send-keys -t py:0.0 "nvim hello.py" C-m \;\
        send-keys -t py:0.1 "micropython hello.py" C-m'

alias tmux-hello-rb='cd $HOME && \
tmux new-session -s rb \;\
        split-window -v -p 30 \;\
        send-keys -t rb:0.0 "nvim hello.rb" C-m \;\
        send-keys -t rb:0.1 "mruby hello.rb" C-m'

alias tmux-hello-c='cd $HOME && \
tmux new-session -s c \;\
        split-window -v -p 30 \;\
        send-keys -t c:0.0 "nvim hello.c" C-m \;\
        send-keys -t c:0.1 "tcc -run hello.c" C-m'
