# Fixed-env experiment table (auto-generated)

| exp | algo | init | obs | dim | reward | steps | wall min | steps/s | final t_can | best t_can | final succ | final ever_up | final ret | final err | first succ @ steps | final robust succ | final robust t_can | best robust succ | best robust t_can |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ppo_angle_base_rand | ppo | rand | angle | 24 | base | 31.76M | 40.6 | 13047 | 0.04 | 0.97 | 0 | 1 | 2697.0 | 0.9751 | - | 0.00 | 0.06 | 0.00 | 0.24 |
| ppo_angle_h1_rand | ppo | rand | angle_h1 | 6 | base | 20.97M | 30.4 | 11493 | 0.00 | 0.00 | 0 | 0 | -3526.7 | 2.9346 | - | 0.00 | 0.00 | 0.00 | 0.00 |
| ppo_angle_h8_rand | ppo | rand | angle_h8 | 48 | base | 30.11M | 40.4 | 12417 | 0.05 | 1.31 | 0 | 1 | 2629.5 | 0.8636 | - | 0.00 | 0.05 | 0.00 | 0.15 |
| ppo_angle_mix | ppo | mix | angle | 24 | base | 23.50M | 30.5 | 12847 | 0.00 | 0.25 | 0 | 1 | -13092.4 | 2.8925 | - | 0.00 | 0.00 | 0.00 | 0.10 |
| ppo_full_base_fixed | ppo | fixed | full | 36 | base | 21.93M | 30.5 | 11978 | 0.00 | 1.19 | 0 | 0 | 260.7 | 0.7154 | - | 0.00 | 0.00 | 0.00 | 1.03 |
| ppo_full_base_rand | ppo | rand | full | 36 | base | 35.98M | 45.1 | 13307 | 26.61 | 26.85 | 1 | 1 | 16772.5 | 0.0220 | 11.33M | 1.00 | 25.57 | 1.00 | 26.86 |
| ppo_full_base_rand_s1 | ppo | rand | full | 36 | base | 29.03M | 40.8 | 11872 | 0.23 | 27.17 | 0 | 1 | 5034.6 | 0.2765 | 13.63M | 0.00 | 0.21 | 1.00 | 26.78 |
| ppo_full_base_rand_s2 | ppo | rand | full | 36 | base | 34.21M | 40.3 | 14135 | 0.00 | 28.46 | 0 | 0 | 4804.4 | 0.0820 | 14.40M | 0.00 | 0.00 | 1.00 | 28.03 |
| ppo_full_energy_rand | ppo | rand | full | 36 | energy | 24.50M | 35.5 | 11503 | 0.00 | 0.00 | 0 | 0 | -1948.3 | 2.8125 | - | 0.00 | 0.00 | 0.00 | 0.00 |
| ppo_full_h1_mix | ppo | mix | full_h1 | 9 | base | 21.39M | 30.5 | 11703 | 0.00 | 0.02 | 0 | 0 | -552.6 | 2.2633 | - | 0.00 | 0.00 | 0.00 | 0.00 |
| ppo_full_h1_rand | ppo | rand | full_h1 | 9 | base | 25.42M | 35.3 | 11988 | 0.00 | 0.06 | 0 | 0 | -1701.9 | 2.6674 | - | 0.00 | 0.00 | 0.00 | 0.06 |
| ppo_full_sparse_rand | ppo | rand | full | 36 | sparse | 19.12M | 30.5 | 10445 | 0.00 | 0.00 | 0 | 0 | -0.4 | 3.1021 | - | 0.00 | 0.00 | 0.00 | 0.00 |
| sac_angle_base_rand | sac | rand | angle | 24 | base | 1.42M | 50.6 | 469 | 0.00 | 0.04 | 0 | 0 | -581.4 | 0.6124 | - | 0.00 | 0.01 | 0.00 | 0.00 |
| sac_full_base_fixed | sac | fixed | full | 36 | base | 1.41M | 50.4 | 466 | 27.62 | 28.86 | 1 | 1 | 17697.1 | 0.0324 | 0.52M | 1.00 | 27.49 | 1.00 | 23.45 |
| sac_full_base_rand | sac | rand | full | 36 | base | 1.42M | 50.6 | 470 | 28.21 | 28.75 | 1 | 1 | 17607.2 | 0.0293 | 0.70M | 1.00 | 26.39 | 0.75 | 20.89 |

## TensorBoard coverage

| exp | tags | required missing | last global_steps | last elapsed min |
|---|---|---|---|---|
| ppo_angle_base_rand | 62 | none | 31756800 | 40.04 |
| ppo_angle_h1_rand | 62 | none | 20966400 | 30.02 |
| ppo_angle_h8_rand | 62 | none | 30105600 | 40.04 |
| ppo_angle_mix | 62 | none | 23500800 | 30.02 |
| ppo_full_base_fixed | 62 | none | 21926400 | 30.01 |
| ppo_full_base_rand | 53 | none | 35980800 | 45.06 |
| ppo_full_base_rand_s1 | 62 | none | 29030400 | 40.02 |
| ppo_full_base_rand_s2 | 62 | none | 34214400 | 40.02 |
| ppo_full_energy_rand | 62 | none | 24499200 | 35.02 |
| ppo_full_h1_mix | 62 | none | 21388800 | 30.04 |
| ppo_full_h1_rand | 62 | none | 25420800 | 35.00 |
| ppo_full_sparse_rand | 61 | none | 19123200 | 30.05 |
| sac_angle_base_rand | 66 | none | 1423040 | 49.99 |
| sac_full_base_fixed | 66 | none | 1407680 | 49.99 |
| sac_full_base_rand | 66 | none | 1424320 | 50.00 |
