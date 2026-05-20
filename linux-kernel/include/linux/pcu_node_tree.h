/*
 * PCU node combining tree definitions.  These are used to compute
 * global attributes while avoiding common-case global contention.  A key
 * property that these computations rely on is a tournament-style approach
 * where only one of the tasks contending a lower level in the tree need
 * advance to the next higher level.  If properly configured, this allows
 * unlimited scalability while maintaining a constant level of contention
 * on the root node.
 *
 * This seemingly PCU-private file must be available to SPCU users
 * because the size of the TREE SPCU srcu_struct structure depends
 * on these definitions.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 * Copyright IBM Corporation, 2017
 *
 * Author: Paul E. McKenney <paulmck@linux.vnet.ibm.com>
 */

#ifndef __LINUX_PCU_NODE_TREE_H
#define __LINUX_PCU_NODE_TREE_H

/*
 * Define shape of hierarchy based on NR_CPUS, CONFIG_PCU_FANOUT, and
 * CONFIG_PCU_FANOUT_LEAF.
 * In theory, it should be possible to add more levels straightforwardly.
 * In practice, this did work well going from three levels to four.
 * Of course, your mileage may vary.
 */

#ifdef CONFIG_PCU_FANOUT
#define PCU_FANOUT CONFIG_PCU_FANOUT
#else /* #ifdef CONFIG_PCU_FANOUT */
# ifdef CONFIG_64BIT
# define PCU_FANOUT 64
# else
# define PCU_FANOUT 32
# endif
#endif /* #else #ifdef CONFIG_PCU_FANOUT */

#ifdef CONFIG_PCU_FANOUT_LEAF
#define PCU_FANOUT_LEAF CONFIG_PCU_FANOUT_LEAF
#else /* #ifdef CONFIG_PCU_FANOUT_LEAF */
#define PCU_FANOUT_LEAF 16
#endif /* #else #ifdef CONFIG_PCU_FANOUT_LEAF */

#define PCU_FANOUT_1	      (PCU_FANOUT_LEAF)
#define PCU_FANOUT_2	      (PCU_FANOUT_1 * PCU_FANOUT)
#define PCU_FANOUT_3	      (PCU_FANOUT_2 * PCU_FANOUT)
#define PCU_FANOUT_4	      (PCU_FANOUT_3 * PCU_FANOUT)

#if NR_CPUS <= PCU_FANOUT_1
#  define PCU_NUM_LVLS	      1
#  define NUM_PCU_LVL_0	      1
#  define NUM_PCU_NODES	      NUM_PCU_LVL_0
#  define NUM_PCU_LVL_INIT    { NUM_PCU_LVL_0 }
#  define PCU_NODE_NAME_INIT  { "rcu_node_0" }
#  define PCU_FQS_NAME_INIT   { "rcu_node_fqs_0" }
#elif NR_CPUS <= PCU_FANOUT_2
#  define PCU_NUM_LVLS	      2
#  define NUM_PCU_LVL_0	      1
#  define NUM_PCU_LVL_1	      DIV_ROUND_UP(NR_CPUS, PCU_FANOUT_1)
#  define NUM_PCU_NODES	      (NUM_PCU_LVL_0 + NUM_PCU_LVL_1)
#  define NUM_PCU_LVL_INIT    { NUM_PCU_LVL_0, NUM_PCU_LVL_1 }
#  define PCU_NODE_NAME_INIT  { "rcu_node_0", "rcu_node_1" }
#  define PCU_FQS_NAME_INIT   { "rcu_node_fqs_0", "rcu_node_fqs_1" }
#elif NR_CPUS <= PCU_FANOUT_3
#  define PCU_NUM_LVLS	      3
#  define NUM_PCU_LVL_0	      1
#  define NUM_PCU_LVL_1	      DIV_ROUND_UP(NR_CPUS, PCU_FANOUT_2)
#  define NUM_PCU_LVL_2	      DIV_ROUND_UP(NR_CPUS, PCU_FANOUT_1)
#  define NUM_PCU_NODES	      (NUM_PCU_LVL_0 + NUM_PCU_LVL_1 + NUM_PCU_LVL_2)
#  define NUM_PCU_LVL_INIT    { NUM_PCU_LVL_0, NUM_PCU_LVL_1, NUM_PCU_LVL_2 }
#  define PCU_NODE_NAME_INIT  { "rcu_node_0", "rcu_node_1", "rcu_node_2" }
#  define PCU_FQS_NAME_INIT   { "rcu_node_fqs_0", "rcu_node_fqs_1", "rcu_node_fqs_2" }
#elif NR_CPUS <= PCU_FANOUT_4
#  define PCU_NUM_LVLS	      4
#  define NUM_PCU_LVL_0	      1
#  define NUM_PCU_LVL_1	      DIV_ROUND_UP(NR_CPUS, PCU_FANOUT_3)
#  define NUM_PCU_LVL_2	      DIV_ROUND_UP(NR_CPUS, PCU_FANOUT_2)
#  define NUM_PCU_LVL_3	      DIV_ROUND_UP(NR_CPUS, PCU_FANOUT_1)
#  define NUM_PCU_NODES	      (NUM_PCU_LVL_0 + NUM_PCU_LVL_1 + NUM_PCU_LVL_2 + NUM_PCU_LVL_3)
#  define NUM_PCU_LVL_INIT    { NUM_PCU_LVL_0, NUM_PCU_LVL_1, NUM_PCU_LVL_2, NUM_PCU_LVL_3 }
#  define PCU_NODE_NAME_INIT  { "rcu_node_0", "rcu_node_1", "rcu_node_2", "rcu_node_3" }
#  define PCU_FQS_NAME_INIT   { "rcu_node_fqs_0", "rcu_node_fqs_1", "rcu_node_fqs_2", "rcu_node_fqs_3" }
#else
# error "CONFIG_PCU_FANOUT insufficient for NR_CPUS"
#endif /* #if (NR_CPUS) <= PCU_FANOUT_1 */

#endif /* __LINUX_PCU_NODE_TREE_H */
