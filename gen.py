import random

# Generate random 1s and 0s and store them in a file
file_path = 'x_values.txt'

NUM_FEATURES = 500

NUM_SAMPLES = 2500

with open(file_path, 'w') as file:
    for _ in range(NUM_SAMPLES):
        # Generate a line with 35 random 1s and 0s
        line = ' '.join(str(random.choice([0, 1])) for _ in range(NUM_FEATURES))

        # Write the line to the file
        file.write(line + '\n')

print(f"Random data has been generated and stored in {file_path}.")


file_path = 'y_values.txt'

with open(file_path, 'w') as file:
    for _ in range(NUM_SAMPLES):
        # Generate a random 1 or 0 and write it to the file
        random_bit = random.choice([0, 1])
        file.write(str(random_bit) + '\n')