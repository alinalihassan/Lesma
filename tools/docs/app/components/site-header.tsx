'use client';

import { Fragment, useState, type ComponentProps, type ReactNode } from 'react';
import { buttonVariants } from 'fumadocs-ui/components/ui/button';
import {
  NavigationMenu,
  NavigationMenuContent,
  NavigationMenuItem,
  NavigationMenuLink,
  NavigationMenuList,
  NavigationMenuTrigger,
  NavigationMenuViewport,
} from 'fumadocs-ui/components/ui/navigation-menu';
import { useDocsLayout } from 'fumadocs-ui/layouts/docs';
import { useHomeLayout } from 'fumadocs-ui/layouts/home';
import {
  LinkItem,
  type BaseSlots,
  type LinkItemType,
  type NavOptions,
} from 'fumadocs-ui/layouts/shared';
import { navItemVariants } from 'fumadocs-ui/layouts/home/slots/header';
import { useIsScrollTop } from 'fumadocs-ui/utils/use-is-scroll-top';
import { cn } from '@/lib/cn';
import Link from 'fumadocs-core/link';
import { ChevronDown, Languages, SidebarIcon } from 'lucide-react';

function isSecondary(item: LinkItemType) {
  if ('secondary' in item && item.secondary != null) {
    return item.secondary;
  }
  return item.type === 'icon';
}

function NavigationMenuLinkItem({
  item,
  ...props
}: { item: LinkItemType } & ComponentProps<typeof NavigationMenuItem>) {
  if (item.type === 'custom') {
    return item.children;
  }
  if (item.type === 'menu') {
    const children = item.items.map((child, j) => {
      if (child.type === 'custom') {
        return <Fragment key={j}>{child.children}</Fragment>;
      }
      const {
        banner = child.icon ? (
          <div className="w-fit rounded-md border bg-fd-muted p-1 [&_svg]:size-4">{child.icon}</div>
        ) : null,
        ...rest
      } = child.menu ?? {};
      return (
        <NavigationMenuLink key={`${j}-${child.url}`} asChild>
          <Link
            href={child.url}
            external={child.external}
            {...rest}
            className={cn(
              'flex flex-col gap-2 rounded-lg border bg-fd-card p-3 transition-colors hover:bg-fd-accent/80 hover:text-fd-accent-foreground',
              rest.className,
            )}
          >
            {rest.children ?? (
              <>
                {banner}
                <p className="text-base font-medium">{child.text}</p>
                <p className="text-sm text-fd-muted-foreground empty:hidden">{child.description}</p>
              </>
            )}
          </Link>
        </NavigationMenuLink>
      );
    });
    return (
      <NavigationMenuItem {...props}>
        <NavigationMenuTrigger className={cn(navItemVariants(), 'rounded-md')}>
          {item.url ? (
            <Link href={item.url} external={item.external}>
              {item.text}
            </Link>
          ) : (
            item.text
          )}
        </NavigationMenuTrigger>
        <NavigationMenuContent className="grid grid-cols-1 gap-2 p-4 md:grid-cols-2 lg:grid-cols-3">
          {children}
        </NavigationMenuContent>
      </NavigationMenuItem>
    );
  }
  return (
    <NavigationMenuItem {...props}>
      <NavigationMenuLink asChild>
        <LinkItem
          item={item}
          aria-label={item.type === 'icon' ? item.label : undefined}
          className={cn(navItemVariants({ variant: item.type }))}
        >
          {item.type === 'icon' ? (
            item.icon
          ) : item.type === 'main' || item.type === 'button' ? (
            <>
              {item.icon}
              {item.text}
            </>
          ) : (
            item.text
          )}
        </LinkItem>
      </NavigationMenuLink>
    </NavigationMenuItem>
  );
}

function MobileNavigationMenuLinkItem({
  item,
  ...props
}: { item: LinkItemType } & ComponentProps<'div'>) {
  if (item.type === 'custom') {
    return <div className={cn('grid', props.className)}>{item.children}</div>;
  }
  if (item.type === 'menu') {
    const header = (
      <>
        {item.icon}
        {item.text}
      </>
    );
    return (
      <div className={cn('mb-4 flex flex-col', props.className)}>
        <p className="mb-1 text-sm text-fd-muted-foreground">
          {item.url ? (
            <NavigationMenuLink asChild>
              <Link href={item.url} external={item.external}>
                {header}
              </Link>
            </NavigationMenuLink>
          ) : (
            header
          )}
        </p>
        {item.items.map((child, i) => (
          <MobileNavigationMenuLinkItem key={i} item={child} />
        ))}
      </div>
    );
  }
  return (
    <NavigationMenuLink asChild>
      <LinkItem
        item={item}
        className={cn(
          {
            main: 'inline-flex items-center gap-2 py-1.5 transition-colors hover:text-fd-popover-foreground/50 data-[active=true]:font-medium data-[active=true]:text-fd-primary [&_svg]:size-4',
            icon: buttonVariants({ size: 'icon', color: 'ghost' }),
            button: buttonVariants({ color: 'secondary', className: 'gap-1.5 [&_svg]:size-4' }),
          }[item.type ?? 'main'],
          props.className,
        )}
        aria-label={item.type === 'icon' ? item.label : undefined}
      >
        {item.icon}
        {item.type === 'icon' ? undefined : item.text}
      </LinkItem>
    </NavigationMenuLink>
  );
}

type SiteHeaderSlots = Pick<
  BaseSlots,
  'navTitle' | 'themeSwitch' | 'searchTrigger' | 'languageSelect'
>;

export type LesmaTopBarProps = {
  nav?: NavOptions;
  navItems: LinkItemType[];
  menuItems: LinkItemType[];
  slots: SiteHeaderSlots;
  /** Docs: mobile sidebar control before the title */
  leading?: ReactNode;
  /** Extra classes (e.g. docs grid area + sticky offset), merged after the slot `className` */
  headerClassName?: string;
  /** Attributes from the layout `slots.header` component props */
  headerSlotProps?: ComponentProps<'header'>;
};

function LesmaTopBar({
  nav,
  navItems,
  menuItems,
  slots,
  leading,
  headerClassName,
  headerSlotProps,
}: LesmaTopBarProps) {
  const [value, setValue] = useState('');
  const transparentMode = nav?.transparentMode ?? 'none';
  const isTop = useIsScrollTop({ enabled: transparentMode === 'top' }) ?? true;
  const isTransparent = transparentMode === 'top' ? isTop : transparentMode === 'always';
  const { className: slotClass, ...slotRest } = headerSlotProps ?? {};

  return (
    <NavigationMenu value={value} onValueChange={setValue} asChild>
      <header
        id="nd-nav"
        {...slotRest}
        className={cn('sticky top-0 z-40 h-14', slotClass, headerClassName)}
      >
        <div
          className={cn(
            'border-b backdrop-blur-lg transition-colors *:mx-auto *:max-w-(--fd-layout-width)',
            value.length > 0 && 'max-lg:rounded-b-2xl max-lg:shadow-lg',
            (!isTransparent || value.length > 0) && 'bg-fd-background/80',
          )}
        >
          <NavigationMenuList className="flex h-14 w-full items-center px-4" asChild>
            <nav>
              {leading}
              {slots.navTitle && (
                <slots.navTitle className="inline-flex items-center gap-2.5 font-semibold" />
              )}
              {nav?.children}
              <ul className="flex flex-row items-center gap-2 px-6 max-sm:hidden">
                {navItems
                  .filter((item) => !isSecondary(item))
                  .map((item, i) => (
                    <NavigationMenuLinkItem key={i} item={item} className="text-sm" />
                  ))}
              </ul>
              <div className="flex max-lg:hidden flex-1 flex-row items-center justify-end gap-1.5">
                {slots.searchTrigger && (
                  <slots.searchTrigger.full
                    hideIfDisabled
                    className="w-full max-w-[240px] rounded-full ps-2.5"
                  />
                )}
                {slots.themeSwitch && <slots.themeSwitch />}
                {slots.languageSelect && (
                  <slots.languageSelect.root>
                    <Languages className="size-5" />
                  </slots.languageSelect.root>
                )}
                <ul className="flex flex-row items-center gap-2 empty:hidden">
                  {navItems.filter(isSecondary).map((item, i) => (
                    <NavigationMenuLinkItem
                      key={i}
                      item={item}
                      className={cn(item.type === 'icon' && '-mx-1 first:ms-0 last:me-0')}
                    />
                  ))}
                </ul>
              </div>
              <div className="-me-1.5 ms-auto flex flex-row items-center lg:hidden">
                {slots.searchTrigger && (
                  <slots.searchTrigger.sm hideIfDisabled className="p-2" />
                )}
                <NavigationMenuItem asChild>
                  <div>
                    <NavigationMenuTrigger
                      aria-label="Toggle Menu"
                      className={cn(
                        buttonVariants({
                          size: 'icon',
                          color: 'ghost',
                          className: 'group [&_svg]:size-5.5',
                        }),
                      )}
                      onPointerMove={
                        (nav as { enableHoverToOpen?: boolean } | undefined)?.enableHoverToOpen
                          ? undefined
                          : (e) => e.preventDefault()
                      }
                    >
                      <ChevronDown className="transition-transform duration-300 group-data-[state=open]:rotate-180" />
                    </NavigationMenuTrigger>
                    <NavigationMenuContent className="flex flex-col p-4 sm:flex-row sm:items-center sm:justify-end">
                      {menuItems
                        .filter((item) => !isSecondary(item))
                        .map((item, i) => (
                          <MobileNavigationMenuLinkItem key={i} item={item} className="sm:hidden" />
                        ))}
                      <div className="-ms-1.5 flex max-sm:mt-2 flex-row items-center gap-2">
                        {menuItems
                          .filter(isSecondary)
                          .map((item, i) => (
                            <MobileNavigationMenuLinkItem
                              key={i}
                              item={item}
                              className={cn(item.type === 'icon' && '-mx-1 first:ms-0')}
                            />
                          ))}
                        <div role="separator" className="flex-1" />
                        {slots.languageSelect && (
                          <slots.languageSelect.root>
                            <Languages className="size-5" />
                            {slots.languageSelect.text && <slots.languageSelect.text />}
                            <ChevronDown className="size-3 text-fd-muted-foreground" />
                          </slots.languageSelect.root>
                        )}
                        {slots.themeSwitch && <slots.themeSwitch />}
                      </div>
                    </NavigationMenuContent>
                  </div>
                </NavigationMenuItem>
              </div>
            </nav>
          </NavigationMenuList>
          <NavigationMenuViewport />
        </div>
      </header>
    </NavigationMenu>
  );
}

export function HomeSiteHeader(props: ComponentProps<'header'>) {
  const { navItems, menuItems, slots, props: { nav } } = useHomeLayout();
  if (nav?.component) {
    return nav.component;
  }
  return (
    <LesmaTopBar
      nav={nav}
      navItems={navItems}
      menuItems={menuItems}
      slots={slots}
      headerSlotProps={props}
    />
  );
}

export function DocsSiteHeader(props: ComponentProps<'header'>) {
  const { slots, props: { nav }, navItems, menuItems } = useDocsLayout();
  if (nav?.component) {
    return nav.component;
  }

  const leading = slots.sidebar ? (
    <slots.sidebar.trigger
      className={cn(
        buttonVariants({ color: 'ghost', size: 'icon-sm', className: 'me-1 p-2 md:hidden' }),
      )}
    >
      <SidebarIcon />
    </slots.sidebar.trigger>
  ) : null;

  return (
    <LesmaTopBar
      nav={nav}
      navItems={navItems}
      menuItems={menuItems}
      slots={slots}
      leading={leading}
      headerSlotProps={props}
      headerClassName="[grid-area:header] top-(--fd-docs-row-1) z-50 layout:[--fd-header-height:--spacing(14)]"
    />
  );
}
