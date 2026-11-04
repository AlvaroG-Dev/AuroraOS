/* gcc/config/aurora.h — Definiciones de AuroraOS para GCC (x86_64-pc-aurora-gnu).
   Plan_GCC_Autocompilacion.md §2.2. Se incluye DESPUES de linux.h: AuroraOS
   usa la ABI Linux/glibc, asi que __linux__ y __gnu_linux__ siguen definidos
   (D3: no se modifican paquetes de terceros) y se anaden __aurora__ y
   __AURORAOS__. */

#undef TARGET_OS_CPP_BUILTINS
#define TARGET_OS_CPP_BUILTINS()		\
  do						\
    {						\
      GNU_USER_TARGET_OS_CPP_BUILTINS ();	\
      builtin_define ("__aurora__");		\
      builtin_define ("__AURORAOS__");		\
      builtin_assert ("system=aurora");		\
    }						\
  while (0)
