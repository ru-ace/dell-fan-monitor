/*
 * i8kmon-ng.c -- Fan monitor and control using i8k kernel module on Dell laptops.
 * Copyright (C) 2019 https://github.com/ru-ace
 * Using code for get/set temp/fan_states from https://github.com/vitorafsr/i8kutils
 * Using code for enable/disable bios fan control from https://github.com/TomFreudenberg/dell-bios-fan-control
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/io.h>
#include <unistd.h>
#include <signal.h>

#include "i8kmon-ng.h"

struct t_cfg cfg = {
    .verbose = false,
    .period = 1000,
    .jump_timeout = 2000,
    .jump_temp_delta = 5,
    .t_low = 45,
    .t_mid = 60,
    .t_high = 80,
    .foolproof_checks = true,
    .daemon = false,
    .bios_disable_version = 0,
    .monitor_only = false,
};

//i8kctl start
int i8k_fd;

void i8k_open()
{
    i8k_fd = open(I8K_PROC, O_RDONLY);
    if (i8k_fd < 0)
    {
        perror("can't open " I8K_PROC);
        exit(EXIT_FAILURE);
    }
}
void i8k_set_fan_state(int fan, int state)
{
    if (cfg.monitor_only)
        return;

    int args[2] = {fan, state};
    if (ioctl(i8k_fd, I8K_SET_FAN, &args) != 0)
    {
        perror("i8k_set_fan_state ioctl error");
        exit(EXIT_FAILURE);
    }
}
int i8k_get_fan_state(int fan)
{
    int args[1] = {fan};
    if (ioctl(i8k_fd, I8K_GET_FAN, &args) == 0)
        return args[0];
    perror("i8k_get_fan_state ioctl error");
    exit(EXIT_FAILURE);
}

int i8k_get_cpu_temp()
{
    int args[1];
    if (ioctl(i8k_fd, I8K_GET_TEMP, &args) == 0)
        return args[0];
    perror("i8k_get_cpu_temp ioctl error");
    exit(EXIT_FAILURE);
}
//i8kctl end

// dell-bios-fan-control start
void init_ioperm()
{
    if (ioperm(0xb2, 4, 1))
    {
        perror("init_ioperm");
        cfg.bios_disable_version = 0;
        exit_failure();
    }
    if (ioperm(0x84, 4, 1))
    {
        perror("init_ioperm");
        cfg.bios_disable_version = 0;
        exit_failure();
    }
}
int i8k_smm(struct smm_regs *regs)
{
    int rc;
    int eax = regs->eax;

    asm volatile("pushq %%rax\n\t"
                 "movl 0(%%rax),%%edx\n\t"
                 "pushq %%rdx\n\t"
                 "movl 4(%%rax),%%ebx\n\t"
                 "movl 8(%%rax),%%ecx\n\t"
                 "movl 12(%%rax),%%edx\n\t"
                 "movl 16(%%rax),%%esi\n\t"
                 "movl 20(%%rax),%%edi\n\t"
                 "popq %%rax\n\t"
                 "out %%al,$0xb2\n\t"
                 "out %%al,$0x84\n\t"
                 "xchgq %%rax,(%%rsp)\n\t"
                 "movl %%ebx,4(%%rax)\n\t"
                 "movl %%ecx,8(%%rax)\n\t"
                 "movl %%edx,12(%%rax)\n\t"
                 "movl %%esi,16(%%rax)\n\t"
                 "movl %%edi,20(%%rax)\n\t"
                 "popq %%rdx\n\t"
                 "movl %%edx,0(%%rax)\n\t"
                 "pushfq\n\t"
                 "popq %%rax\n\t"
                 "andl $1,%%eax\n"
                 : "=a"(rc)
                 : "a"(regs)
                 : "%ebx", "%ecx", "%edx", "%esi", "%edi");

    if (rc != 0 || (regs->eax & 0xffff) == 0xffff || regs->eax == eax)
        return -1;

    return 0;
}
int send_smm(unsigned int cmd, unsigned int arg)
{

    struct smm_regs regs = {
        .eax = cmd,
    };

    regs.ebx = arg;
    int res = i8k_smm(&regs);
    if (cfg.verbose)
    {
        printf("i8k_smm returns %d\n", res);
        printf("send_smm returns %d\n", regs.eax);
    }
    return regs.eax;
}

void bios_fan_control(int enable)
{
    if (geteuid() != 0)
    {
        printf("For using \"bios_disable_version\" you need root privileges\n");
        cfg.bios_disable_version = 0;
        exit_failure();
    }
    init_ioperm();
    if (cfg.bios_disable_version == 1)
    {
        if (enable)
            send_smm(ENABLE_BIOS_METHOD1, 0);
        else
            send_smm(DISABLE_BIOS_METHOD1, 0);
    }
    else if (cfg.bios_disable_version == 2)
    {
        if (enable)
            send_smm(ENABLE_BIOS_METHOD2, 0);
        else
            send_smm(DISABLE_BIOS_METHOD2, 0);
    }
    else
    {
        printf("bios_disable_version can be 0 (dont try disable bios), 1 or 2");
        exit_failure();
    }
}
// dell-bios-fan-control end

// i8kmon-ng start
void monitor()
{
    int fan = I8K_FAN_OFF;
    int temp_prev = i8k_get_cpu_temp();
    int skip_once = 0;
    if (cfg.verbose)
    {

        puts("Config:");
        printf("  period                %ld ms\n", cfg.period);
        printf("  jump_timeout          %ld ms\n", cfg.jump_timeout);
        printf("  jump_temp_delta       %d°\n", cfg.jump_temp_delta);
        printf("  t_low                 %d°\n", cfg.t_low);
        printf("  t_mid                 %d°\n", cfg.t_mid);
        printf("  t_high                %d°\n", cfg.t_high);
        printf("  bios_disable_version  %d\n", cfg.bios_disable_version);
        puts("Legend:");
        puts("  [TT/L/R] Monitor(no action). TT - CPU temp, L - left fan state, R - right fan state");
        puts("  [ --F--] Set fans state to F. F - fan state 0 = OFF, 1 = LOW, 2 = HIGH.");
        puts("  [  TT  ] Abnormal temp jump detected. Will wait for next value to make decision. TT - CPU temp. ");
        puts("Monitor:");
    }
    if (cfg.monitor_only)
    {
        puts("WARNING: working in monitor_only mode. No action will taken.");
        fan = i8k_get_fan_state(I8K_FAN_LEFT);
    }
    while (1)
    {
        int temp = i8k_get_cpu_temp();
        int fan_left = i8k_get_fan_state(I8K_FAN_LEFT);
        int fan_right = i8k_get_fan_state(I8K_FAN_RIGHT);
        if (temp - temp_prev > cfg.jump_temp_delta && skip_once == 0)
        {
            if (cfg.verbose)
            {
                printf("  %d   ", temp);
                fflush(stdout);
            }
            skip_once = 1;

            usleep(cfg.jump_timeout * 1000);
            continue;
        }
        else
        {
            if (cfg.verbose)
                printf("%d/%d/%d ", temp, fan_left, fan_right);
        }
        if (temp <= cfg.t_low)
            fan = I8K_FAN_OFF;
        else if (temp > cfg.t_high)
            fan = I8K_FAN_HIGH;
        else if (temp >= cfg.t_mid)
            fan = fan == I8K_FAN_HIGH ? fan : I8K_FAN_LOW;

        if (fan != fan_left || fan != fan_right)
        {
            i8k_set_fan_state(I8K_FAN_LEFT, fan);
            i8k_set_fan_state(I8K_FAN_RIGHT, fan);
            if (cfg.verbose)
                printf(" --%d-- ", fan);
        }
        temp_prev = temp;
        skip_once = 0;
        if (cfg.verbose)
            fflush(stdout);
        usleep(cfg.period * 1000);
    }
}

void foolproof_checks()
{
    int check_failed = false;
    check_failed += (cfg.t_low < 30) ? foolproof_error("t_low >= 30") : false;
    check_failed += (cfg.t_high > 90) ? foolproof_error("t_high <= 90") : false;
    check_failed += (cfg.t_low < cfg.t_mid && cfg.t_mid < cfg.t_high) ? false : foolproof_error("thresholds t_low < t_mid < t_high");
    check_failed += (cfg.period < 100 || cfg.period > 5000) ? foolproof_error("period in [100,5000]") : false;
    check_failed += (cfg.jump_timeout < 100 || cfg.jump_timeout > 5000) ? foolproof_error("jump_timeout in [100,5000]") : false;
    check_failed += (cfg.jump_temp_delta < 2) ? foolproof_error("jump_temp_delta > 2") : false;
    check_failed += (cfg.bios_disable_version < 0 || cfg.bios_disable_version > 2) ? foolproof_error("bios_disable_version in [0,2]") : false;

    if (check_failed)
    {
        printf("foolproof_checks() was failed.\nIf you are sure what you do, checks can be disabled using \"--foolproof_checks 0\" in command line or add \"foolproof_checks 0\" in %s\n", CFG_FILE);
        exit_failure();
    }
}

int foolproof_error(char *str)
{
    printf("foolproof_checks(): awaiting %s\n", str);
    return true;
}

void exit_failure()
{
    i8k_set_fan_state(I8K_FAN_LEFT, I8K_FAN_HIGH);
    i8k_set_fan_state(I8K_FAN_RIGHT, I8K_FAN_HIGH);
    if (cfg.bios_disable_version == 1 || cfg.bios_disable_version == 2)
        bios_fan_control(true);
    exit(EXIT_FAILURE);
}

void cfg_load()
{
    if (access(CFG_FILE, F_OK) == -1)
        return;

    FILE *fh;
    if ((fh = fopen(CFG_FILE, "r")) == NULL)
        return;

    char *str = malloc(256);
    int line_id = 0;
    while (!feof(fh))
        if (fgets(str, 254, fh))
        {
            line_id++;
            char *pos = strstr(str, "\n");
            pos[0] = '\0';
            //printf("%s: [%s]\n", CFG_FILE, str);
            if (str[0] == '#' || str[0] == ';' || str[0] == '\0' || isspace(str[0]))
                continue;
            pos = str;
            while (!isspace(pos[0]) && pos[0] != '\0')
                pos++;

            if (pos[0] == '\0')
                cfg_error(line_id);

            pos[0] = '\0';
            pos++;
            while (!isdigit(pos[0]) && pos[0] != '\0')
                pos++;

            if (pos[0] == '\0')
                cfg_error(line_id);

            int value = atoi(pos);
            cfg_set(str, value, line_id);
        }

    free(str);
    fclose(fh);
}

void cfg_error(int line_id)
{
    printf("Error while loading " CFG_FILE " in line %d\n", line_id);
    exit_failure();
}

void cfg_set(char *key, int value, int line_id)
{
    if (cfg.verbose)
        printf("%s = %d\n", key, value);

    if (strcmp(key, "daemon") == 0)
        cfg.daemon = value;
    else if (strcmp(key, "period") == 0)
        cfg.period = value;
    else if (strcmp(key, "jump_timeout") == 0)
        cfg.jump_timeout = value;
    else if (strcmp(key, "jump_temp_delta") == 0)
        cfg.jump_temp_delta = value;
    else if (strcmp(key, "t_low") == 0)
        cfg.t_low = value;
    else if (strcmp(key, "t_mid") == 0)
        cfg.t_mid = value;
    else if (strcmp(key, "t_high") == 0)
        cfg.t_high = value;
    else if (strcmp(key, "foolproof_checks") == 0)
        cfg.foolproof_checks = value;
    else if (strcmp(key, "bios_disable_version") == 0)
        cfg.bios_disable_version = value;
    else
    {
        if (line_id > 0)
            printf(CFG_FILE " line %d: unknown parameter %s \n", line_id, key);
        else
            printf("Unknown argument --%s\n", key);

        exit_failure();
    }
}

void usage()
{
    puts("i8kmon-ng v0.9 by https://github.com/ru-ace");
    puts("Fan monitor and control using i8k kernel module on Dell laptops.\n");

    puts("Usage: i8kmon-ng [OPTIONS]");
    puts("  -h  Show this help");
    puts("  -v  Verbose mode");
    puts("  -d  Daemon mode (detach from console)");
    puts("  -m  No control - monitor only (useful to monitor daemon working)");
    printf("Args(see %s for explains):\n", CFG_FILE);
    printf("  --period MILLISECONDS             (default: %ld ms)\n", cfg.period);
    printf("  --jump_timeout MILLISECONDS       (default: %ld ms)\n", cfg.jump_timeout);
    printf("  --jump_temp_delta CELSIUS         (default: %d°)\n", cfg.jump_temp_delta);
    printf("  --t_low CELSIUS                   (default: %d°)\n", cfg.t_low);
    printf("  --t_mid CELSIUS                   (default: %d°)\n", cfg.t_mid);
    printf("  --t_high CELSIUS                  (default: %d°)\n", cfg.t_high);
    printf("  --bios_disable_version VERSION    (default: %d)\n", cfg.bios_disable_version);

    puts("");
}
void parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        if ((strcmp(argv[i], "-h") == 0) || (strcmp(argv[i], "--help") == 0))
        {
            usage();
            exit(EXIT_SUCCESS);
        }
        else if ((strcmp(argv[i], "-v") == 0) || (strcmp(argv[i], "--verbose") == 0))
        {
            cfg.verbose = true;
        }
        else if ((strcmp(argv[i], "-d") == 0) || (strcmp(argv[i], "--daemon") == 0))
        {
            cfg.daemon = true;
        }
        else if ((strcmp(argv[i], "-m") == 0) || (strcmp(argv[i], "--monitor_only") == 0))
        {
            cfg.monitor_only = true;
            cfg.verbose = true;
        }
        else if (argv[i][0] == '-' && argv[i][1] == '-')
        {
            char *key = argv[i];
            key += 2;
            i++;
            if (i == argc || !isdigit(argv[i][0]))
            {
                printf("Argument --%s need value\n", key);
                exit_failure();
            }
            else
            {
                int value = atoi(argv[i]);
                cfg_set(key, value, 0);
            }
        }
        else
        {
            printf("Unknown argument %s\n", argv[i]);
            exit_failure();
        }
    }
}

void daemonize()
{
    int pid = fork();
    if (pid == -1) //failure
    {
        perror("can't fork()");
        exit_failure();
    }
    else if (pid == 0) //child
        cfg.verbose = false;
    else
    { //parent
        printf("%d\n", pid);
        exit(EXIT_SUCCESS);
    }
}
void signal_handler(int signal_id)
{
    if (cfg.verbose)
        printf("\nCatch signal %d. Exiting...\n", signal_id);
    exit_failure();
}

void signal_handler_init()
{
    if (signal(SIGINT, signal_handler) == SIG_ERR)
    {
        perror("can't catch SIGINT");
        exit_failure();
    }
    if (signal(SIGTERM, signal_handler) == SIG_ERR)
    {
        perror("can't catch SIGTERM");
        exit_failure();
    }
}

int main(int argc, char **argv)
{
    i8k_open();
    cfg_load();
    parse_args(argc, argv);
    if (cfg.verbose)
    {
        puts("i8kmon-ng v0.9 by https://github.com/ru-ace");
        puts("Fan monitor and control using i8k kernel module on Dell laptops.\n");
    }
    signal_handler_init();
    if (cfg.foolproof_checks)
        foolproof_checks();
    if (cfg.bios_disable_version && !cfg.monitor_only)
        bios_fan_control(false);
    if (cfg.daemon && !cfg.monitor_only)
        daemonize();

    monitor();
}