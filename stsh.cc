/**
 * File: stsh.cc
 * -------------
 * Defines the entry point of the stsh executable.
 */

#include <iostream>
#include <fcntl.h>
#include <unistd.h>   // for fork
#include <sys/wait.h> // for waitpid
#include "stsh-parser/stsh-parse.h"
#include "stsh-parser/stsh-readline.h"
#include "stsh-exception.h"
#include "fork-utils.h" // this needs to be the last #include in the list

using namespace std;

// inputRedirection function takes a pipeline object as an argument and handles input redirection if necessary
void inputRedirection(const pipeline& p) {
  // Store the input file name from the pipeline object
  string input = p.input; 
  
  // Check if the input file name is not empty
  if (!input.empty()) {
    // Open the input file for reading with permissions 0644
    int input_fd = open(input.c_str(), O_RDONLY, 0644);
    
    // Check if the file was successfully opened
    if (input_fd == -1) {
      // If the file could not be opened, throw an exception with a message
      throw STSHException("Could not open \"" + input + "\".");
    }
    
    // Redirect the standard input to the input file
    dup2(input_fd, STDIN_FILENO);
    
    // Close the input file
    close(input_fd);
  } 
}

// outputRedirection function takes a pipeline object as an argument and handles output redirection if necessary
void outputRedirection(const pipeline& p) {
  // Store the output file name from the pipeline object
  string output = p.output;
  
  // Check if the output file name is not empty
  if (!output.empty()) {
    // Open the output file for reading and writing with permissions 0644
    int output_fd = open(output.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    
    // Check if the file was successfully opened
    if (output_fd == -1) {
      // If the file could not be opened, throw an exception with a message
      throw STSHException("Could not open " + output + ".");
    }
    
    // Redirect the standard output to the output file
    dup2(output_fd, STDOUT_FILENO);
    
    // Close the output file
    close(output_fd);
  }  
}

// waitOrSegFault function takes a child process ID as an argument and waits for them all
// Also errors if segmentation fault in any of the child processes
void waitOrSegFault(pid_t child) {
  // Store the status of the child process
  int status;
  
  // Wait for the child process to complete
  waitpid(child, &status, 0);
  
  // Check if the child process was terminated by a signal
  if (WIFSIGNALED(status)) {
    // Get the number of the signal that terminated the child process
    int signal_num = WTERMSIG(status);
    
    // Check if the signal was SIGSEGV (segmentation fault)
    if (signal_num == SIGSEGV) {
      // If the signal was SIGSEGV, print a message to the error stream
      cerr << "Segmentation fault\n";
    }
  }
}

// singleCommand function takes a pipeline object as an argument and handles the circumstance when the 
// user only inputs one command into the shell.
void singleCommand(const pipeline& p) {
  // Get the first command in the pipeline
  command cmd = p.commands[0];
  
  // Create a child process
  pid_t child_pid = fork();
  
  // Check if the child process was successfully created
  if (child_pid == 0) {
    // If the child process was created, perform input redirection
    inputRedirection(p);
    
    // Perform output redirection
    outputRedirection(p);
    
    // Execute the command
    execvp(cmd.argv[0], cmd.argv);
    
    // If the command could not be executed, throw an exception with a message
    throw STSHException(string(cmd.argv[0]) + ": Command not found.");
  }
  
  // Wait for the child process to complete and check if it terminated due to a segmentation fault
  waitOrSegFault(child_pid);
}

// twoProcessPipelines function performs a two process pipeline if the user inputs two commands. 
// It takes in the pipeline 'p' and the number of commands 'num_commands' as input. 
void twoProcessPipelines(const pipeline& p, int num_commands) {
// Extract the first and second command from the pipeline 'p'
  command cmd1 = p.commands[0];
  command cmd2 = p.commands[1];

  // Declare an array 'children' to store the process IDs
  pid_t children[2];

  // Declare an array 'fds' to store the file descriptors for the pipe
  int fds[2];
  // Call the 'pipe2' function to create a pipe with the specified flags
  pipe2(fds, O_CLOEXEC);

  // Fork the first child process
  children[0] = fork();
  // If the child process is the first child
  if (children[0] == 0) {
    // Call the 'inputRedirection' function to handle input redirection
    inputRedirection(p);
    // Duplicate the write end of the pipe to the standard output
    dup2(fds[1], STDOUT_FILENO);
    // Close the read end of the pipe
    close(fds[0]);
    // Execute the first command
    execvp(cmd1.argv[0], cmd1.argv);
    // If the command is not found, throw an exception
    throw STSHException(string(cmd1.argv[0]) + ": Command not found.\n");
  }
  // Close the write end of the pipe in the parent process
  close(fds[1]);

  // Fork the second child process
  children[1] = fork();
  // If the child process is the second child
  if (children[1] == 0) {
    // Call the 'outputRedirection' function to handle output redirection
    outputRedirection(p);
    // Duplicate the read end of the pipe to the standard input
    dup2(fds[0], STDIN_FILENO);
    // Execute the second command
    execvp(cmd2.argv[0], cmd2.argv);
    // If the command is not found, throw an exception
    throw STSHException(string(cmd2.argv[0]) + ": Command not found.\n");
  }

  // Wait for the two child processes to finish
  for (int i = 0; i < num_commands; i++) {
    pid_t curr_child = children[i];
    waitOrSegFault(curr_child);
  }
}

// Handles the circumstance if the user inputs multiple commands connected with each other using pipelines
void multiProcessPipelines(const pipeline& p, int num_commands) {
// Array of file descriptors for pipes to be created between commands
  int fds[num_commands - 1][2];

  // Array to store child processes' process IDs
  pid_t children[num_commands];

  // A variable to manage which pipes to use
  int pipe_manager = 0;

  // Create pipes between commands
  for (int i = 0; i < num_commands - 1; i++) {
      // The pipe2 function creates a pipe and sets the O_CLOEXEC flag
      pipe2(fds[i], O_CLOEXEC);
  }

  // Loop over each command in the pipeline
  for (int i = 0; i < num_commands; i++) {
    // Current command
    command curr_cmd = p.commands[i];

    // Check if this is the first command
    if (i == 0) {
      // Create a child process for the first command
      children[i] = fork();
      // Check if this process is the child process
      if (children[i] == 0) {
        // Perform input redirection, if any
        inputRedirection(p);
        // Connect the standard output of this command to the write end of the pipe
        dup2(fds[pipe_manager][1], STDOUT_FILENO);
        // Close the read end of the pipe
        close(fds[pipe_manager][0]);
        // Execute the command
        execvp(curr_cmd.argv[0], curr_cmd.argv);
        // If the execution of the command fails, throw an exception with an error message
        throw STSHException(string(curr_cmd.argv[0]) + ": Command not found.");
      }
      // Close the write end of the pipe from the parent process
      close(fds[pipe_manager][1]);
      // Skip to the next iteration of the loop
      continue;
    }
    
    // Check if this is the last command
    if (i == num_commands - 1) {
      // Create a child process for the last command
      children[i] = fork();
      // Check if this process is the child process
      if (children[i] == 0) {
        // Perform output redirection, if any
        outputRedirection(p);
        // Connect the standard input of this command to the read end of the pipe
        dup2(fds[pipe_manager][0], STDIN_FILENO);
        // Execute the command
        execvp(curr_cmd.argv[0], curr_cmd.argv);
        // If the execution of the command fails, throw an exception with an error message
        throw STSHException(string(curr_cmd.argv[0]) + ": Command not found.");
      }
      // Close the read end of the pipe from the parent process
      close(fds[pipe_manager][0]);
      // Skip to the next iteration of the loop
      continue;

    } else {  // any commands in between
      // Create a child process for the current command
      children[i] = fork();
      // Check if this process is the child process
      if (children[i] == 0) {
        // Connect the standard input of this command to the read end of the pipe
        dup2(fds[pipe_manager][0], STDIN_FILENO);
        pipe_manager++;
        dup2(fds[pipe_manager][1], STDOUT_FILENO);
        // Execute the command
        execvp(curr_cmd.argv[0], curr_cmd.argv);
        // If the execution of the command fails, throw an exception with an error message
        throw STSHException(string(curr_cmd.argv[0]) + ": Command not found.");
      }
      // Close the read end of the pipe from the parent process
      close(fds[pipe_manager][0]); 
      pipe_manager++;
      close(fds[pipe_manager][1]);
    }
  }

  // Wait for all child processes to finish and throw an exception if there is a segmentation fault
  for (int i = 0; i < num_commands; i++) {
    pid_t curr_child = children[i];
    waitOrSegFault(curr_child);
  }  
}

// Main function that calls all the helper/decomposed functions above
void runPipeline(const pipeline& p) {
  int num_commands = p.commands.size();
  if (num_commands == 1) singleCommand(p);  // if only one command
  if (num_commands == 2) twoProcessPipelines(p, num_commands);  // if strictly two commands
  if (num_commands > 2) multiProcessPipelines(p, num_commands);  // if multiple commands
}

/**
 * Define the entry point for a process running stsh.
 */
int main(int argc, char *argv[]) {
  pid_t stshpid = getpid();
  rlinit(argc, argv);
  while (true) {
    string line;
    if (!readline(line) || line == "quit" || line == "exit") break;
    if (line.empty()) continue;
    try {
      pipeline p(line);
      runPipeline(p);
    } catch (const STSHException& e) {
      cerr << e.what() << endl;
      if (getpid() != stshpid) exit(0); // if exception is thrown from child process, kill it
    }
  }
  return 0;
}
