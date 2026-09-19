00000000004015c0 <main>:
  4015c0:	55                   	push   %rbp
  4015c1:	31 f6                	xor    %esi,%esi
  4015c3:	bf 35 1a 40 00       	mov    $0x401a35,%edi
  4015c8:	48 89 f2             	mov    %rsi,%rdx
  4015cb:	48 89 e5             	mov    %rsp,%rbp
  4015ce:	41 57                	push   %r15
  4015d0:	41 bf 01 00 00 00    	mov    $0x1,%r15d
  4015d6:	41 56                	push   %r14
  4015d8:	4c 89 f8             	mov    %r15,%rax
  4015db:	41 55                	push   %r13
  4015dd:	41 54                	push   %r12
  4015df:	53                   	push   %rbx
  4015e0:	48 83 ec 38          	sub    $0x38,%rsp
  4015e4:	49 c7 c2 00 00 00 00 	mov    $0x0,%r10
  4015eb:	49 c7 c0 00 00 00 00 	mov    $0x0,%r8
  4015f2:	0f 05                	syscall
  4015f4:	b8 13 00 00 00       	mov    $0x13,%eax
  4015f9:	bf 50 00 00 00       	mov    $0x50,%edi
  4015fe:	be 3c 00 00 00       	mov    $0x3c,%esi
  401603:	ba 34 03 00 00       	mov    $0x334,%edx
  401608:	49 c7 c2 58 02 00 00 	mov    $0x258,%r10
  40160f:	49 c7 c0 47 1a 40 00 	mov    $0x401a47,%r8
  401616:	0f 05                	syscall
  401618:	31 f6                	xor    %esi,%esi
  40161a:	48 89 c3             	mov    %rax,%rbx
  40161d:	41 89 c6             	mov    %eax,%r14d
  401620:	bf 56 1a 40 00       	mov    $0x401a56,%edi
  401625:	4c 89 f8             	mov    %r15,%rax
  401628:	48 89 f2             	mov    %rsi,%rdx
  40162b:	49 c7 c2 00 00 00 00 	mov    $0x0,%r10
  401632:	49 c7 c0 00 00 00 00 	mov    $0x0,%r8
  401639:	0f 05                	syscall
  40163b:	4c 89 f8             	mov    %r15,%rax
  40163e:	85 db                	test   %ebx,%ebx
  401640:	0f 88 da 01 00 00    	js     401820 <main+0x260>
  401646:	31 f6                	xor    %esi,%esi
  401648:	bf 87 1a 40 00       	mov    $0x401a87,%edi
  40164d:	48 89 f2             	mov    %rsi,%rdx
  401650:	49 c7 c2 00 00 00 00 	mov    $0x0,%r10
  401657:	49 c7 c0 00 00 00 00 	mov    $0x0,%r8
  40165e:	0f 05                	syscall
  401660:	bf 80 6d 1c 00       	mov    $0x1c6d80,%edi
  401665:	4c 63 eb             	movslq %ebx,%r13
  401668:	e8 d3 eb ff ff       	call   400240 <malloc>
  40166d:	49 89 c4             	mov    %rax,%r12
  401670:	48 85 c0             	test   %rax,%rax
  401673:	0f 84 16 02 00 00    	je     40188f <main+0x2cf>
  401679:	e8 12 f6 ff ff       	call   400c90 <buf_init>
  40167e:	bf f0 1c 40 00       	mov    $0x401cf0,%edi
  401683:	e8 98 f8 ff ff       	call   400f20 <buf_puts>
  401688:	bf ac 1a 40 00       	mov    $0x401aac,%edi
  40168d:	e8 8e f8 ff ff       	call   400f20 <buf_puts>
  401692:	bf 20 1d 40 00       	mov    $0x401d20,%edi
  401697:	e8 84 f8 ff ff       	call   400f20 <buf_puts>
  40169c:	bf f0 1c 40 00       	mov    $0x401cf0,%edi
  4016a1:	e8 7a f8 ff ff       	call   400f20 <buf_puts>
  4016a6:	bf 2c 1a 40 00       	mov    $0x401a2c,%edi
  4016ab:	e8 70 f8 ff ff       	call   400f20 <buf_puts>
  4016b0:	4c 89 e7             	mov    %r12,%rdi
  4016b3:	e8 a8 f3 ff ff       	call   400a60 <render.constprop.0>
  4016b8:	31 f6                	xor    %esi,%esi
  4016ba:	89 5d b0             	mov    %ebx,-0x50(%rbp)
  4016bd:	48 8d 7d b0          	lea    -0x50(%rbp),%rdi
  4016c1:	48 b8 34 03 00 00 38 	movabs $0x23800000334,%rax
  4016c8:	02 00 00 
  4016cb:	48 89 75 b4          	mov    %rsi,-0x4c(%rbp)
  4016cf:	31 f6                	xor    %esi,%esi
  4016d1:	48 89 45 bc          	mov    %rax,-0x44(%rbp)
  4016d5:	48 89 f2             	mov    %rsi,%rdx
  4016d8:	b8 15 00 00 00       	mov    $0x15,%eax
  4016dd:	4c 89 65 c8          	mov    %r12,-0x38(%rbp)
  4016e1:	49 c7 c2 00 00 00 00 	mov    $0x0,%r10
  4016e8:	49 c7 c0 00 00 00 00 	mov    $0x0,%r8
  4016ef:	0f 05                	syscall
  4016f1:	b8 17 00 00 00       	mov    $0x17,%eax
  4016f6:	4c 89 ef             	mov    %r13,%rdi
  4016f9:	49 c7 c2 00 00 00 00 	mov    $0x0,%r10
  401700:	49 c7 c0 00 00 00 00 	mov    $0x0,%r8
  401707:	0f 05                	syscall
  401709:	85 c0                	test   %eax,%eax
  40170b:	0f 88 35 01 00 00    	js     401846 <main+0x286>
  401711:	41 bf 16 00 00 00    	mov    $0x16,%r15d
  401717:	ba 01 00 00 00       	mov    $0x1,%edx
  40171c:	4c 89 f8             	mov    %r15,%rax
  40171f:	48 8d 75 a0          	lea    -0x60(%rbp),%rsi
  401723:	4c 89 ef             	mov    %r13,%rdi
  401726:	49 c7 c2 00 00 00 00 	mov    $0x0,%r10
  40172d:	49 c7 c0 00 00 00 00 	mov    $0x0,%r8
  401734:	0f 05                	syscall
  401736:	85 c0                	test   %eax,%eax
  401738:	7e dd                	jle    401717 <main+0x157>
  40173a:	8b 45 a0             	mov    -0x60(%rbp),%eax
  40173d:	83 f8 07             	cmp    $0x7,%eax
  401740:	0f 84 73 01 00 00    	je     4018b9 <main+0x2f9>
  401746:	77 44                	ja     40178c <main+0x1cc>
  401748:	83 f8 01             	cmp    $0x1,%eax
  40174b:	0f 84 99 00 00 00    	je     4017ea <main+0x22a>
  401751:	83 f8 05             	cmp    $0x5,%eax
  401754:	0f 85 6d 01 00 00    	jne    4018c7 <main+0x307>
  40175a:	8b 45 a4             	mov    -0x5c(%rbp),%eax
  40175d:	83 f8 49             	cmp    $0x49,%eax
  401760:	0f 84 fd 01 00 00    	je     401963 <main+0x3a3>
  401766:	83 f8 51             	cmp    $0x51,%eax
  401769:	0f 84 da 01 00 00    	je     401949 <main+0x389>
  40176f:	83 f8 4f             	cmp    $0x4f,%eax
  401772:	0f 84 de 01 00 00    	je     401956 <main+0x396>
  401778:	45 31 c9             	xor    %r9d,%r9d
  40177b:	83 f8 47             	cmp    $0x47,%eax
  40177e:	75 27                	jne    4017a7 <main+0x1e7>
  401780:	c7 05 42 da 00 00 7f 	movl   $0x98967f,0xda42(%rip)        # 40f1cc <g_buf+0xcc0c>
  401787:	96 98 00 
  40178a:	eb 15                	jmp    4017a1 <main+0x1e1>
  40178c:	83 f8 08             	cmp    $0x8,%eax
  40178f:	0f 85 32 01 00 00    	jne    4018c7 <main+0x307>
  401795:	0f be 7d a4          	movsbl -0x5c(%rbp),%edi
  401799:	44 89 f6             	mov    %r14d,%esi
  40179c:	e8 3f fd ff ff       	call   4014e0 <handle_key>
  4017a1:	41 b9 01 00 00 00    	mov    $0x1,%r9d
  4017a7:	bb 16 00 00 00       	mov    $0x16,%ebx
  4017ac:	48 89 d8             	mov    %rbx,%rax
  4017af:	4c 89 ef             	mov    %r13,%rdi
  4017b2:	48 8d 75 a0          	lea    -0x60(%rbp),%rsi
  4017b6:	31 d2                	xor    %edx,%edx
  4017b8:	49 c7 c2 00 00 00 00 	mov    $0x0,%r10
  4017bf:	49 c7 c0 00 00 00 00 	mov    $0x0,%r8
  4017c6:	0f 05                	syscall
  4017c8:	85 c0                	test   %eax,%eax
  4017ca:	0f 8e ff 00 00 00    	jle    4018cf <main+0x30f>
  4017d0:	8b 45 a0             	mov    -0x60(%rbp),%eax
  4017d3:	83 f8 07             	cmp    $0x7,%eax
  4017d6:	0f 84 a8 00 00 00    	je     401884 <main+0x2c4>
  4017dc:	83 f8 08             	cmp    $0x8,%eax
  4017df:	0f 84 88 00 00 00    	je     40186d <main+0x2ad>
  4017e5:	83 f8 01             	cmp    $0x1,%eax
  4017e8:	75 c2                	jne    4017ac <main+0x1ec>
  4017ea:	31 f6                	xor    %esi,%esi
  4017ec:	b8 14 00 00 00       	mov    $0x14,%eax
  4017f1:	4c 89 ef             	mov    %r13,%rdi
  4017f4:	48 89 f2             	mov    %rsi,%rdx
  4017f7:	49 c7 c2 00 00 00 00 	mov    $0x0,%r10
  4017fe:	49 c7 c0 00 00 00 00 	mov    $0x0,%r8
  401805:	0f 05                	syscall
  401807:	4c 89 e7             	mov    %r12,%rdi
  40180a:	e8 a1 eb ff ff       	call   4003b0 <free>
  40180f:	31 c0                	xor    %eax,%eax
  401811:	48 83 c4 38          	add    $0x38,%rsp
  401815:	5b                   	pop    %rbx
  401816:	41 5c                	pop    %r12
  401818:	41 5d                	pop    %r13
  40181a:	41 5e                	pop    %r14
  40181c:	41 5f                	pop    %r15
  40181e:	5d                   	pop    %rbp
  40181f:	c3                   	ret
  401820:	bf 6e 1a 40 00       	mov    $0x401a6e,%edi
  401825:	49 c7 c2 00 00 00 00 	mov    $0x0,%r10
  40182c:	49 c7 c0 00 00 00 00 	mov    $0x0,%r8
  401833:	0f 05                	syscall
  401835:	bf c8 1c 40 00       	mov    $0x401cc8,%edi
  40183a:	e8 51 ed ff ff       	call   400590 <puts>
  40183f:	b8 01 00 00 00       	mov    $0x1,%eax
  401844:	eb cb                	jmp    401811 <main+0x251>
  401846:	4c 89 f8             	mov    %r15,%rax
  401849:	bf 50 1d 40 00       	mov    $0x401d50,%edi
  40184e:	49 c7 c2 00 00 00 00 	mov    $0x0,%r10
  401855:	49 c7 c0 00 00 00 00 	mov    $0x0,%r8
  40185c:	0f 05                	syscall
  40185e:	bf 70 1d 40 00       	mov    $0x401d70,%edi
  401863:	e8 28 ed ff ff       	call   400590 <puts>
  401868:	e9 a4 fe ff ff       	jmp    401711 <main+0x151>
  40186d:	0f be 7d a4          	movsbl -0x5c(%rbp),%edi
  401871:	44 89 f6             	mov    %r14d,%esi
  401874:	e8 67 fc ff ff       	call   4014e0 <handle_key>
  401879:	41 b9 01 00 00 00    	mov    $0x1,%r9d
  40187f:	e9 28 ff ff ff       	jmp    4017ac <main+0x1ec>
  401884:	0f be 7d a4          	movsbl -0x5c(%rbp),%edi
  401888:	e8 03 f6 ff ff       	call   400e90 <buf_putchar>
  40188d:	eb ea                	jmp    401879 <main+0x2b9>
  40188f:	bf 97 1a 40 00       	mov    $0x401a97,%edi
  401894:	e8 f7 ec ff ff       	call   400590 <puts>
  401899:	b8 14 00 00 00       	mov    $0x14,%eax
  40189e:	4c 89 ef             	mov    %r13,%rdi
  4018a1:	4c 89 e6             	mov    %r12,%rsi
  4018a4:	4c 89 e2             	mov    %r12,%rdx
  4018a7:	49 c7 c2 00 00 00 00 	mov    $0x0,%r10
  4018ae:	49 c7 c0 00 00 00 00 	mov    $0x0,%r8
  4018b5:	0f 05                	syscall
  4018b7:	eb 86                	jmp    40183f <main+0x27f>
  4018b9:	0f be 7d a4          	movsbl -0x5c(%rbp),%edi
  4018bd:	e8 ce f5 ff ff       	call   400e90 <buf_putchar>
  4018c2:	e9 da fe ff ff       	jmp    4017a1 <main+0x1e1>
  4018c7:	45 31 c9             	xor    %r9d,%r9d
  4018ca:	e9 d8 fe ff ff       	jmp    4017a7 <main+0x1e7>
  4018cf:	8b 05 ef d8 00 00    	mov    0xd8ef(%rip),%eax        # 40f1c4 <g_buf+0xcc04>
  4018d5:	83 e8 01             	sub    $0x1,%eax
  4018d8:	0f 48 c2             	cmovs  %edx,%eax
  4018db:	8b 15 eb d8 00 00    	mov    0xd8eb(%rip),%edx        # 40f1cc <g_buf+0xcc0c>
  4018e1:	39 c2                	cmp    %eax,%edx
  4018e3:	7f 5c                	jg     401941 <main+0x381>
  4018e5:	85 d2                	test   %edx,%edx
  4018e7:	79 08                	jns    4018f1 <main+0x331>
  4018e9:	31 d2                	xor    %edx,%edx
  4018eb:	89 15 db d8 00 00    	mov    %edx,0xd8db(%rip)        # 40f1cc <g_buf+0xcc0c>
  4018f1:	45 85 c9             	test   %r9d,%r9d
  4018f4:	0f 84 1d fe ff ff    	je     401717 <main+0x157>
  4018fa:	4c 89 e7             	mov    %r12,%rdi
  4018fd:	e8 5e f1 ff ff       	call   400a60 <render.constprop.0>
  401902:	31 c0                	xor    %eax,%eax
  401904:	31 f6                	xor    %esi,%esi
  401906:	44 89 75 b0          	mov    %r14d,-0x50(%rbp)
  40190a:	48 89 45 b4          	mov    %rax,-0x4c(%rbp)
  40190e:	48 8d 7d b0          	lea    -0x50(%rbp),%rdi
  401912:	48 89 f2             	mov    %rsi,%rdx
  401915:	48 b8 34 03 00 00 38 	movabs $0x23800000334,%rax
  40191c:	02 00 00 
  40191f:	48 89 45 bc          	mov    %rax,-0x44(%rbp)
  401923:	b8 15 00 00 00       	mov    $0x15,%eax
  401928:	4c 89 65 c8          	mov    %r12,-0x38(%rbp)
  40192c:	49 c7 c2 00 00 00 00 	mov    $0x0,%r10
  401933:	49 c7 c0 00 00 00 00 	mov    $0x0,%r8
  40193a:	0f 05                	syscall
  40193c:	e9 d6 fd ff ff       	jmp    401717 <main+0x157>
  401941:	89 05 85 d8 00 00    	mov    %eax,0xd885(%rip)        # 40f1cc <g_buf+0xcc0c>
  401947:	eb a8                	jmp    4018f1 <main+0x331>
  401949:	83 2d 7c d8 00 00 05 	subl   $0x5,0xd87c(%rip)        # 40f1cc <g_buf+0xcc0c>
  401950:	0f 89 4b fe ff ff    	jns    4017a1 <main+0x1e1>
  401956:	31 c9                	xor    %ecx,%ecx
  401958:	89 0d 6e d8 00 00    	mov    %ecx,0xd86e(%rip)        # 40f1cc <g_buf+0xcc0c>
  40195e:	e9 3e fe ff ff       	jmp    4017a1 <main+0x1e1>
  401963:	83 05 62 d8 00 00 05 	addl   $0x5,0xd862(%rip)        # 40f1cc <g_buf+0xcc0c>
  40196a:	e9 32 fe ff ff       	jmp    4017a1 <main+0x1e1>
