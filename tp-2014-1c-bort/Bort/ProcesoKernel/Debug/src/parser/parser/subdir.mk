################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../src/parser/parser/metadata_program.c \
../src/parser/parser/parser.c 

OBJS += \
./src/parser/parser/metadata_program.o \
./src/parser/parser/parser.o 

C_DEPS += \
./src/parser/parser/metadata_program.d \
./src/parser/parser/parser.d 


# Each subdirectory must supply rules for building sources it contributes
src/parser/parser/%.o: ../src/parser/parser/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C Compiler'
	gcc -I"/home/utnso/workspace/parser" -I"/home/utnso/workspace/commons" -O0 -g3 -Wall -pthread -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


